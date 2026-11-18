/*
 * AmperoDisplayPico2 – touch foot controller for the Hotone Ampero II (Stomp) with an HY28B
 * (2.8", ILI9325, SPI) on a Raspberry Pi Pico 2 (RP2350).
 *
 * Shows the preset name, the active scene and six equal tiles (touch footswitches). Each tile is set up
 * in the touch menu: select a scene, toggle between two scenes, switch an effect block on/off or do what
 * footswitch 1/2/3 of the Ampero does; optionally also a MIDI message on MIDI DIN. Two EXP ports
 * (pedal or switch), MIDI DIN in/out with routing.
 *
 * Core 1: PIO USB host (Adafruit TinyUSB + Pico-PIO-USB >= 0.7.3, built-in TinyUSB MIDI host)
 * Core 0: Ampero protocol (AmperoClient), display, touch, menu, EXP ports, MIDI DIN (UART0);
 *         exchange with core 1 through two queues.
 * The Pico 2's micro USB port stays free for programming, power and the serial log (115200).
 *
 * Arduino IDE:
 *   Board:      Raspberry Pi Pico/RP2040/RP2350 (earlephilhower) -> "Raspberry Pi Pico 2"
 *   CPU Architecture: ARM
 *   CPU Speed:  120 MHz            (PIO USB needs 120 or 240 MHz, not the default 150 MHz)
 *   USB Stack:  Adafruit TinyUSB
 * Libraries: Adafruit TinyUSB Library, Pico PIO USB (>= 0.7.3), Adafruit GFX Library
 * IMPORTANT: in Adafruit_TinyUSB_Library/src/arduino/ports/rp2040/tusb_config_rp2040.h set
 *   CFG_TUH_ENUMERATION_BUFSIZE from 256 to 1024 – the Ampero's configuration descriptor is
 *   505 bytes long, otherwise enumeration fails (see README).
 */
#include "pio_usb.h"
#include <Adafruit_TinyUSB.h>
#include "host/hcd.h"   // HCD_EVENT_* for the enumeration log
#include <functional>
#include <hardware/clocks.h>
#include <pico/util/queue.h>

#if !defined(CFG_TUH_MIDI) || (CFG_TUH_MIDI == 0)
#error "TinyUSB MIDI host is not enabled (CFG_TUH_MIDI) - select USB Stack 'Adafruit TinyUSB'."
#endif
#if CFG_TUH_ENUMERATION_BUFSIZE < 512
#error "CFG_TUH_ENUMERATION_BUFSIZE must be >= 512 (Ampero configuration descriptor: 505 bytes). See comment above / README."
#endif

#include "AmperoClient.h"
#include "Config.h"
#include "Controller.h"
#include "Ili9325Spi.h"
#include "Menu.h"
#include "MidiDin.h"
#include "MidiParser.h"
#include "Screen.h"
#include "StripCanvas.h"
#include "Xpt2046.h"

// ------------------------------------------------------------------ configuration

// USB host (PIO): D+ and D- must be neighbouring GPIOs, in either order
constexpr uint8_t PIN_USB_DP = 13;   // GP13 = D+
constexpr uint8_t PIN_USB_DM = 12;   // GP12 = D-
static_assert(PIN_USB_DM == PIN_USB_DP + 1 || PIN_USB_DM + 1 == PIN_USB_DP,
              "D+/D- must be neighbouring GPIOs");

// HY28B (SPI0, default pins of the Pico 2)
constexpr Ili9325Spi::Pins LCD_PINS = {
    /*sck*/ 18, /*mosi*/ 19, /*miso*/ 16, /*cs*/ 17, /*reset*/ 20, /*backlight*/ 21};
constexpr int LCD_ROTATION = 90;            // 90/270 = landscape (320x240), 0/180 = portrait (240x320)
constexpr uint32_t LCD_SPI_HZ = 30000000;   // fbtft runs the HY28B at up to 48 MHz; image errors -> 16000000
constexpr uint8_t LCD_BRIGHTNESS = 255;     // 0..255 (PWM on BL_CTRL)

// HY28B touch (XPT2046) on SPI1 – own bus, SPI mode 0 at 2 MHz
constexpr Xpt2046::Pins TOUCH_PINS = {
    /*sck*/ 10, /*mosi*/ 11, /*miso*/ 8, /*cs*/ 9, /*irq*/ 22};
constexpr uint32_t TILE_HOLD_MS = 1000;     // hold a tile this long -> its settings page

// MIDI-DIN on UART0 (Serial1), EXP ports on ADC0/ADC1
constexpr uint8_t PIN_MIDI_TX = 0;          // GP0 -> MIDI OUT (pin 5 via 10 ohm)
constexpr uint8_t PIN_MIDI_RX = 1;          // GP1 <- optocoupler of MIDI IN
constexpr Controller::ExpPins EXP_PINS = {{26, 27}};   // GP26 = EXP 1 (tip), GP27 = EXP 2 (tip)

constexpr int LCD_W = (LCD_ROTATION == 90 || LCD_ROTATION == 270) ? 320 : 240;
constexpr int LCD_H = (LCD_ROTATION == 90 || LCD_ROTATION == 270) ? 240 : 320;
constexpr int STRIP_ROWS = 30;

// ------------------------------------------------------------------ shared between the cores

struct SysExMsg {
  uint16_t len;
  uint8_t data[400];   // Ampero frames are at most 384 bytes; also short MIDI messages
};
static queue_t rxQueue;   // core1 -> core0
static queue_t txQueue;   // core0 -> core1
static volatile bool queuesReady = false;
static volatile bool forwardAmperoMidi = false;   // core 1 also queues non-SysEx messages of the Ampero
static volatile int midiIdx = -1;           // TinyUSB MIDI index of the Ampero, -1 = none
static volatile uint32_t usbEpoch = 0;      // changes on every mount/unmount
static volatile uint16_t lastVid = 0, lastPid = 0;
static volatile uint32_t mountCount = 0;
static volatile bool clockError = false;
static volatile bool usbStarted = false;
// enumeration progress, for the diagnostic log
static volatile uint32_t attachCount = 0, removeCount = 0, devDescCount = 0, cfgDescCount = 0;
static volatile uint16_t enumVid = 0, enumPid = 0, enumBcdUsb = 0, enumCfgLen = 0;
static volatile uint8_t enumMaxPacket0 = 0;
// MIDI interface as found by TinyUSB (for the log)
static volatile uint32_t midiDescCount = 0;
static volatile uint8_t midiItf = 0, midiEpIn = 0, midiEpOut = 0, midiRxCables = 0, midiTxCables = 0;
static volatile uint16_t midiEpInSize = 0, midiEpOutSize = 0;
// MIDI traffic counters (core 1 writes, core 0 logs)
static volatile uint32_t midiTxTimeouts = 0;
static volatile uint32_t midiTxBytes = 0, midiRxBytes = 0, midiRxSysEx = 0, midiRxDropped = 0;

// ------------------------------------------------------------------ core 0: protocol + display

static Ili9325Spi lcd;
static StripCanvas canvas(LCD_W, LCD_H, STRIP_ROWS);
static AmperoClient ampero;
static Xpt2046 touch;
static bool touchCalibrated = false;
static AppConfig cfg;
static MidiDin din;
static Controller ctl;
static Menu menu;

static bool sendFrame(const uint8_t* f, size_t n) {
  if (midiIdx < 0 || n == 0 || n > sizeof(SysExMsg::data)) return false;
  static SysExMsg m;   // static: 400 bytes are too much for every call frame
  m.len = (uint16_t)n;
  memcpy(m.data, f, n);
  return queue_try_add(&txQueue, &m);
}

/**
 * Draws one frame; render(gfx) is called once per strip. Only strips whose content changed since
 * the last frame are sent to the display (hash per strip), so small changes cost little SPI time.
 */
static void present(const std::function<void(Adafruit_GFX&)>& render) {
  static uint32_t stripHash[(LCD_H + STRIP_ROWS - 1) / STRIP_ROWS];
  static bool valid = false;
  const uint32_t t0 = micros();
  int sent = 0, s = 0;
  for (int y = 0; y < LCD_H; y += STRIP_ROWS, s++) {
    canvas.setStrip(y);
    render(canvas);
    const size_t n = (size_t)LCD_W * canvas.validRows();
    const uint32_t* w = (const uint32_t*)canvas.buffer();
    uint32_t h = 2166136261u;   // FNV-1a over 32-bit words
    for (size_t i = 0; i < n / 2; i++) h = (h ^ w[i]) * 16777619u;
    if (valid && h == stripHash[s]) continue;
    stripHash[s] = h;
    lcd.beginFrameAt(y);
    lcd.writePixels(canvas.buffer(), n);   // byte-swaps the buffer (after hashing)
    sent++;
  }
  valid = true;
  Serial.printf("[%lu] Display: %lu ms, %d/%d strips sent\r\n", (unsigned long)millis(),
                (unsigned long)((micros() - t0) / 1000), sent, s);
}

static void draw() {
  if (menu.isOpen()) {
    static MenuView v;
    menu.build(v);
    present([&](Adafruit_GFX& g) { renderMenu(g, v); });
    return;
  }
  static ScreenState s;
  s.usbConnected = midiIdx >= 0;
  s.online = ampero.online();
  s.hasPreset = ampero.hasPreset();
  s.presetIndex = ampero.presetIndex();
  s.presetName = ampero.presetName();
  s.sceneName = ampero.activeScene() >= 0 ? ampero.sceneName(ampero.activeScene()) : "";
  for (int i = 0; i < 6; i++) ctl.tileView(i, s.tiles[i]);
  present([&](Adafruit_GFX& g) { renderScreen(g, s); });
}

/** Settings changed: apply what depends on them. */
static void applyConfig() {
  ampero.setMidiChannel(cfg.midi.amperoChannel);
  forwardAmperoMidi = cfg.midi.amperoToDin;
}

// ------------------------------------------------------------------ MIDI-DIN

static void onDinMessage(const uint8_t* m, size_t n) {
  if (cfg.midi.dinThru) din.send(m, n);
  if (cfg.midi.dinToAmpero && midiIdx >= 0) sendFrame(m, n);
}

// ------------------------------------------------------------------ touch

/** Waits for a press, returns the averaged raw position after release; false on timeout. */
static bool captureRawPoint(int& rx, int& ry, uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (!touch.penDown()) {
    if (millis() - t0 > timeoutMs) return false;
    delay(10);
  }
  long sx = 0, sy = 0;
  int n = 0;
  uint32_t up = 0;
  while (true) {                       // sample while pressed, stop 80 ms after release
    int x, y;
    if (touch.penDown() && touch.readRaw(x, y)) {
      sx += x; sy += y; n++;
      up = 0;
    } else if (!up) {
      up = millis();
    } else if (millis() - up > 80) {
      break;
    }
    delay(5);
  }
  if (n < 3) return false;
  rx = (int)(sx / n);
  ry = (int)(sy / n);
  return true;
}

/** 3-point calibration (targets near three corners), stored in flash. */
static bool calibrateTouch() {
  const int px[3] = {30, LCD_W - 30, LCD_W / 2};
  const int py[3] = {30, LCD_H / 2, LCD_H - 30};
  int rx[3], ry[3];
  while (touch.penDown()) delay(10);   // finger from the boot-time press must be lifted first
  for (int i = 0; i < 3; i++) {
    present([&](Adafruit_GFX& g) { renderCalibration(g, i, px[i], py[i], "Tap the centre of the cross (stylus)"); });
    if (!captureRawPoint(rx[i], ry[i], 30000)) {
      Serial.println("Touch: calibration cancelled (no input)");
      return false;
    }
    Serial.printf("Touch: point %d -> raw %d/%d\r\n", i + 1, rx[i], ry[i]);
    delay(250);
  }
  TouchCalibration c;
  if (!Xpt2046::solve(px, py, rx, ry, c)) {
    present([&](Adafruit_GFX& g) { renderCalibration(g, 2, -1, -1, "Failed - points too close"); });
    delay(1500);
    return false;
  }
  touch.setCalibration(c);
  Xpt2046::saveCalibration(c);
  Serial.printf("Touch: calibrated a=%.4f b=%.4f c=%.1f d=%.4f e=%.4f f=%.1f\r\n", c.a, c.b, c.c, c.d, c.e, c.f);
  return true;
}

static void handleMenuResult(Menu::Result r) {
  switch (r) {
    case Menu::REDRAW:
      draw();
      break;
    case Menu::CLOSED:
      if (menu.changed()) {
        configSave(cfg);   // flash write: core 1 pauses briefly
        applyConfig();
        Serial.println("Settings saved");
      }
      draw();
      break;
    case Menu::CALIBRATE_TOUCH:
      touchCalibrated = calibrateTouch() || touchCalibrated;
      draw();
      break;
    default:
      break;
  }
}

/**
 * Main screen: tap on a tile = its function (on touch down, like a footswitch), holding it
 * TILE_HOLD_MS = its settings page, tap on the status bar = menu. Menu: tap, arrows repeat while held.
 */
static void pollTouch() {
  static bool down = false, longDone = false;
  static int pressedTile = -1;
  static uint32_t downSince = 0, lastPoll = 0, lastRepeat = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 15) return;
  lastPoll = now;

  int rx, ry;
  const bool pressed = touch.penDown() && touch.readRaw(rx, ry);
  if (!pressed) {
    if (down && !touch.penDown()) {   // released
      down = false;
      if (pressedTile >= 0) ctl.release(pressedTile);
      pressedTile = -1;
    }
    return;
  }
  if (!touchCalibrated) return;
  int x, y;
  touch.map(rx, ry, x, y);

  if (!down) {                        // new press
    down = true;
    longDone = false;
    downSince = lastRepeat = now;
    if (menu.isOpen()) {
      handleMenuResult(menu.press(LCD_W, LCD_H, x, y));
    } else if (statusBarAt(LCD_W, LCD_H, x, y)) {
      menu.open();
      draw();
    } else {
      pressedTile = tileAt(LCD_W, LCD_H, x, y);
      Serial.printf("Touch: %d/%d -> tile %d\r\n", x, y, pressedTile + 1);
      if (pressedTile >= 0) ctl.press(pressedTile);
    }
    return;
  }
  // held
  if (menu.isOpen()) {
    if (menu.repeatable() && now - downSince > 450 && now - lastRepeat > 90) {
      lastRepeat = now;
      handleMenuResult(menu.repeat());
    }
  } else if (pressedTile >= 0 && !longDone && now - downSince > TILE_HOLD_MS) {
    longDone = true;
    ctl.release(pressedTile);
    menu.open(pressedTile);
    pressedTile = -1;
    draw();
  }
}

void setup() {
  queue_init(&rxQueue, sizeof(SysExMsg), 32);
  queue_init(&txQueue, sizeof(SysExMsg), 16);

  Serial.begin(115200);
  if (!canvas.ok()) Serial.println("Error: no RAM for the strip buffer");
  if (!configLoad(cfg)) Serial.println("Settings: defaults");
  applyConfig();
  queuesReady = true;

  din.begin(PIN_MIDI_TX, PIN_MIDI_RX);
  ctl.begin(&cfg, &ampero, &din, EXP_PINS);
  menu.begin(&cfg, &ampero, &ctl);

  lcd.begin(LCD_PINS, LCD_ROTATION, LCD_SPI_HZ);
  touch.begin(SPI1, TOUCH_PINS);
  draw();
  lcd.setBacklight(LCD_BRIGHTNESS);

  // calibrate when nothing is stored yet or the display is pressed while switching on
  TouchCalibration cal;
  const bool stored = Xpt2046::loadCalibration(cal);
  if (stored) touch.setCalibration(cal);
  delay(300);
  if (!stored || touch.penDown()) touchCalibrated = calibrateTouch() || stored;
  else touchCalibrated = true;
  draw();

  ampero.begin(sendFrame);
}

/** Logs the USB line state and every enumeration step (attach, descriptors, mount). */
static void usbDiagnostics() {
  static uint32_t lastPoll = 0, lastLines = 0;
  static uint32_t shownAttach = 0, shownRemove = 0, shownDev = 0, shownCfg = 0;
  static int shownLines = -1;
  static bool startShown = false;
  const uint32_t now = millis();
  if (now - lastPoll < 100) return;
  lastPoll = now;

  if (usbStarted && !startShown) {
    startShown = true;
    Serial.printf("USB host started: D+ GP%u, D- GP%u, CPU %lu MHz\r\n", PIN_USB_DP, PIN_USB_DM,
                  (unsigned long)(clock_get_hz(clk_sys) / 1000000));
  }
  // line state only while nothing is attached (during traffic the lines toggle constantly).
  // Pico-PIO-USB inverts both inputs (gpio_set_inover), so gpio_get() returns the inverted level.
  if (attachCount == removeCount) {
    const int lines = (gpio_get(PIN_USB_DP) ? 0 : 1) | (gpio_get(PIN_USB_DM) ? 0 : 2);
    if (lines != shownLines || now - lastLines > 5000) {
      lastLines = now;
      static const char* const kState[] = {"both low -> no device (check VBUS/cable/pins)",
                                           "D+ high -> full-speed device detected",
                                           "D- high -> low-speed device or D+/D- swapped",
                                           "both high -> short circuit/error"};
      Serial.printf("USB lines: D+=%d D-=%d (%s)\r\n", lines & 1, lines >> 1, kState[lines]);
      shownLines = lines;
    }
  } else {
    shownLines = -1;
  }
  if (attachCount != shownAttach) {
    shownAttach = attachCount;
    Serial.println("USB: device attached, enumeration starts");
  }
  if (devDescCount != shownDev) {
    shownDev = devDescCount;
    Serial.printf("USB: device descriptor %04x:%04x, USB %x.%02x, EP0 %u bytes\r\n", enumVid, enumPid,
                  enumBcdUsb >> 8, enumBcdUsb & 0xFF, enumMaxPacket0);
  }
  if (cfgDescCount != shownCfg) {
    shownCfg = cfgDescCount;
    Serial.printf("USB: configuration descriptor %u bytes (buffer %u)\r\n", enumCfgLen,
                  (unsigned)CFG_TUH_ENUMERATION_BUFSIZE);
  }
  if (removeCount != shownRemove) {
    shownRemove = removeCount;
    Serial.println("USB: device removed");
  }
  static uint32_t shownMidiDesc = 0;
  if (midiDescCount != shownMidiDesc && midiIdx >= 0) {   // after mount: cable counts are set
    shownMidiDesc = midiDescCount;
    Serial.printf("MIDI interface %u: EP IN %02X (%u bytes), EP OUT %02X (%u bytes), cables rx %u / tx %u\r\n",
                  midiItf, midiEpIn, midiEpInSize, midiEpOut, midiEpOutSize, midiRxCables, midiTxCables);
  }
  // status every 5 s until the first preset is shown
  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 5000 && !ampero.hasPreset()) {
    lastStatus = now;
    Serial.printf("Status: attached %lu/removed %lu, mounted %lu, MIDI index %d, sent %lu, received %lu bytes\r\n",
                  (unsigned long)attachCount, (unsigned long)removeCount, (unsigned long)mountCount, (int)midiIdx,
                  (unsigned long)midiTxBytes, (unsigned long)midiRxBytes);
  }
  // MIDI traffic every 2 s while it changes
  static uint32_t lastMidi = 0, shownTx = 0, shownRx = 0, shownDinRx = 0, shownDinTx = 0;
  if (now - lastMidi >= 2000 && (midiTxBytes != shownTx || midiRxBytes != shownRx || din.rxCount() != shownDinRx ||
                                 din.txCount() != shownDinTx)) {
    lastMidi = now;
    shownTx = midiTxBytes;
    shownRx = midiRxBytes;
    shownDinRx = din.rxCount();
    shownDinTx = din.txCount();
    Serial.printf("MIDI: USB sent %lu bytes, received %lu bytes, %lu SysEx (%lu dropped), send timeouts %lu; "
                  "DIN in %lu, out %lu messages\r\n",
                  (unsigned long)shownTx, (unsigned long)shownRx, (unsigned long)midiRxSysEx,
                  (unsigned long)midiRxDropped, (unsigned long)midiTxTimeouts, (unsigned long)shownDinRx,
                  (unsigned long)shownDinTx);
  }
}

/** Hex dump of the first received SysEx messages after each connect. */
static void logRxSysEx(const SysExMsg& m, bool reset) {
  static int shown = 0;
  if (reset) shown = 0;
  if (shown >= 6) return;
  shown++;
  Serial.printf("MIDI rx %u bytes:", m.len);
  for (int i = 0; i < m.len && i < 24; i++) Serial.printf(" %02X", m.data[i]);
  Serial.println(m.len > 24 ? " ..." : "");
}

void loop() {
  static uint32_t epoch = 0, drawnRevision = 0xFFFFFFFF, drawnCtl = 0xFFFFFFFF, lastDraw = 0, shownMounts = 0;
  static bool drawnConnected = false, clockReported = false;
  static SysExMsg m;

  usbDiagnostics();

  if (clockError && !clockReported) {
    clockReported = true;
    Serial.printf("Error: CPU clock %lu Hz - set Tools -> CPU Speed to 120 or 240 MHz\r\n",
                  (unsigned long)clock_get_hz(clk_sys));
  }
  if (mountCount != shownMounts) {
    shownMounts = mountCount;
    Serial.printf("USB: device %04x:%04x mounted\r\n", lastVid, lastPid);
  }
  if (usbEpoch != epoch) {
    epoch = usbEpoch;
    if (midiIdx >= 0) {
      Serial.printf("USB MIDI connected (index %d)\r\n", midiIdx);
      ampero.onConnected();
    } else {
      Serial.println("USB MIDI disconnected");
      ampero.onDisconnected();
    }
  }

  static uint32_t loggedEpoch = 0;
  while (queue_try_remove(&rxQueue, &m)) {
    if (m.data[0] == 0xF0) {
      logRxSysEx(m, loggedEpoch != usbEpoch);
      loggedEpoch = usbEpoch;
      ampero.onSysEx(m.data, m.len);
    } else if (cfg.midi.amperoToDin) {
      din.send(m.data, m.len);   // e.g. CC/PC/clock the Ampero sends
    }
  }
  din.poll(onDinMessage);
  ampero.loop();
  ctl.poll();
  pollTouch();

  const bool connected = midiIdx >= 0;
  const uint32_t now = millis();
  if (menu.isOpen()) {
    // live value on the calibration page
    static uint32_t drawnExp = 0;
    if (menu.page() == Menu::PAGE_EXP_CAL && ctl.expRevision() != drawnExp && now - lastDraw > 150) {
      drawnExp = ctl.expRevision();
      lastDraw = now;
      draw();
    }
    return;
  }
  if ((ampero.revision() != drawnRevision || ctl.revision() != drawnCtl || connected != drawnConnected) &&
      now - lastDraw > 30) {
    drawnRevision = ampero.revision();
    drawnCtl = ctl.revision();
    drawnConnected = connected;
    lastDraw = now;
    draw();
  }
}

// ------------------------------------------------------------------ core 1: USB host

Adafruit_USBH_Host USBHost;

static MidiParser<400> usbParser;

static void pushByte(uint8_t b) {
  if (!usbParser.feed(b)) return;
  const uint8_t* msg = usbParser.msg();
  const size_t len = usbParser.len();
  if (msg[0] != 0xF0 && !forwardAmperoMidi) return;   // only SysEx unless routed to MIDI-DIN
  static SysExMsg m;
  m.len = (uint16_t)len;
  memcpy(m.data, msg, len);
  if (msg[0] == 0xF0) midiRxSysEx = midiRxSysEx + 1;
  if (!queue_try_add(&rxQueue, &m)) midiRxDropped = midiRxDropped + 1;   // core 0 is behind
}

void setup1() {
  while (!queuesReady) delay(1);
  const uint32_t hz = clock_get_hz(clk_sys);
  if (hz != 120000000UL && hz != 240000000UL) {
    clockError = true;
    while (true) delay(1000);
  }
  pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  cfg.pin_dp = PIN_USB_DP;
  cfg.pinout = PIN_USB_DM == PIN_USB_DP + 1 ? PIO_USB_PINOUT_DPDM : PIO_USB_PINOUT_DMDP;
  USBHost.configure_pio_usb(1, &cfg);
  USBHost.begin(1);
  usbStarted = true;
}

// The Ampero stops accepting OUT data after a zero-length packet, and TinyUSB sends one after every
// transfer of exactly 64 bytes (the editor never does). So messages are packed into USB-MIDI packets
// here and sent in transfers of at most 15 packets (60 bytes), one at a time.
static volatile bool txInFlight = false;
constexpr int kTxChunkPackets = 15;
constexpr uint32_t kTxTimeoutMs = 500;

void loop1() {
  static SysExMsg tx;
  static size_t txOff = 0;
  static bool txBusy = false;
  static uint32_t txSince = 0;

  USBHost.task();

  const int idx = midiIdx;
  if (idx < 0) {
    txBusy = false;
    txInFlight = false;
    return;
  }
  if (txInFlight) {
    if (millis() - txSince < kTxTimeoutMs) return;
    txInFlight = false;                        // no completion: give up waiting
    midiTxTimeouts = midiTxTimeouts + 1;
  }
  if (!txBusy && queue_try_remove(&txQueue, &tx)) {
    txBusy = true;
    txOff = 0;
  }
  if (!txBusy) return;

  uint8_t pk[4 * kTxChunkPackets];
  size_t n = 0, off = txOff;
  const uint8_t st = tx.data[0];
  if (st != 0xF0) {                            // anything but SysEx: one packet
    uint8_t cin;
    if (st < 0xF0) cin = st >> 4;              // channel message: CIN = status >> 4
    else if (st >= 0xF8) cin = 0x0F;           // realtime: single byte
    else cin = tx.len == 1 ? 0x05 : tx.len == 2 ? 0x02 : 0x03;   // system common
    pk[0] = cin;
    for (size_t i = 0; i < 3; i++) pk[1 + i] = i < tx.len ? tx.data[i] : 0;
    n = 4;
    off = tx.len;
  }
  while (off < tx.len && n < sizeof pk) {
    const size_t left = tx.len - off;
    const size_t take = left > 3 ? 3 : left;
    const bool end = tx.data[off + take - 1] == 0xF7;
    pk[n] = end ? (uint8_t)(0x04 + take) : 0x04;   // CIN 4 = SysEx continues, 5/6/7 = ends with 1/2/3 bytes
    for (size_t i = 0; i < 3; i++) pk[n + 1 + i] = i < take ? tx.data[off + i] : 0;
    n += 4;
    off += take;
  }
  const uint32_t w = tuh_midi_packet_write_n((uint8_t)idx, pk, (uint32_t)n);
  if (w != n) return;                          // FIFO not empty yet: retry next loop
  txInFlight = true;
  txSince = millis();
  tuh_midi_write_flush((uint8_t)idx);
  midiTxBytes = midiTxBytes + (off - txOff);
  txOff = off;
  if (txOff >= tx.len) txBusy = false;
}

// TinyUSB host callbacks (run on core 1 inside USBHost.task())
extern "C" {

// may run in interrupt context: only count
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
  (void)rhport;
  (void)in_isr;
  if (eventid == HCD_EVENT_DEVICE_ATTACH) attachCount = attachCount + 1;
  else if (eventid == HCD_EVENT_DEVICE_REMOVE) removeCount = removeCount + 1;
}

void tuh_enum_descriptor_device_cb(uint8_t daddr, const tusb_desc_device_t* d) {
  (void)daddr;
  enumVid = d->idVendor;
  enumPid = d->idProduct;
  enumBcdUsb = d->bcdUSB;
  enumMaxPacket0 = d->bMaxPacketSize0;
  devDescCount = devDescCount + 1;
}

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index, const tusb_desc_configuration_t* c) {
  (void)daddr;
  (void)cfg_index;
  enumCfgLen = c->wTotalLength;
  cfgDescCount = cfgDescCount + 1;
  return true;
}

void tuh_mount_cb(uint8_t dev_addr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  lastVid = vid;
  lastPid = pid;
  mountCount = mountCount + 1;
}

void tuh_midi_descriptor_cb(uint8_t idx, const tuh_midi_descriptor_cb_t* d) {
  (void)idx;
  if (d->desc_midi) midiItf = d->desc_midi->bInterfaceNumber;
  if (d->desc_epin) {
    midiEpIn = d->desc_epin->bEndpointAddress;
    midiEpInSize = tu_edpt_packet_size(d->desc_epin);
  }
  if (d->desc_epout) {
    midiEpOut = d->desc_epout->bEndpointAddress;
    midiEpOutSize = tu_edpt_packet_size(d->desc_epout);
  }
  midiDescCount = midiDescCount + 1;
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* mount_cb_data) {
  midiRxCables = mount_cb_data->rx_cable_count;
  midiTxCables = mount_cb_data->tx_cable_count;
  usbParser.reset();
  midiIdx = idx;
  usbEpoch = usbEpoch + 1;
}

void tuh_midi_tx_cb(uint8_t idx, uint32_t xferred_bytes) {
  (void)idx;
  (void)xferred_bytes;
  txInFlight = false;
}

void tuh_midi_umount_cb(uint8_t idx) {
  if (midiIdx == idx) {
    midiIdx = -1;
    usbEpoch = usbEpoch + 1;
  }
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
  (void)xferred_bytes;
  uint8_t buf[64];
  uint8_t cable = 0;
  uint32_t n;
  // tuh_midi_stream_read may write up to 3 bytes beyond the size it is given
  while ((n = tuh_midi_stream_read(idx, &cable, buf, sizeof buf - 3)) > 0) {
    midiRxBytes = midiRxBytes + n;
    for (uint32_t i = 0; i < n; i++) pushByte(buf[i]);
  }
}

}  // extern "C"
