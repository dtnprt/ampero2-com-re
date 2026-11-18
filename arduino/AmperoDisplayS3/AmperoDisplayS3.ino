/*
 * AmperoDisplayS3 – touch foot controller for the Hotone Ampero II (Stomp) on an ESP32-S3 DevKitC-1
 * (N16R8) with a MAX3421E USB host board and a 3.5" IPS display (ST7796U, 320x480, SPI) with
 * capacitive touch (FT6336U, I2C).
 *
 * Shows the preset name, the active scene and six equal tiles (touch footswitches). Each tile is set up
 * in the touch menu: select a scene, toggle between two scenes, switch an effect block on/off or do what
 * footswitch 1/2/3 of the Ampero does; optionally also a MIDI message on MIDI DIN. Two EXP ports
 * (pedal or switch), MIDI DIN in/out with routing.
 *
 * Core 1 (Arduino loop): USB host on the MAX3421E (Adafruit TinyUSB, built-in MIDI host), together with
 *         the library's MAX3421 interrupt task
 * Core 0 (task "ui"): Ampero protocol (AmperoClient), display, touch, menu, EXP ports, MIDI DIN (UART1);
 *         exchange with core 1 through two FreeRTOS queues.
 * The ESP32-S3's own USB ports stay free for programming, power and the serial log (115200).
 *
 * Arduino IDE:
 *   Board:   esp32 (Espressif, >= 3.0) -> "ESP32S3 Dev Module"
 *   Flash Size 16MB, PSRAM "OPI PSRAM", USB Mode "Hardware CDC and JTAG" (defaults otherwise)
 * Libraries: Adafruit TinyUSB Library (>= 3.4), TFT_eSPI (>= 2.5.43), Adafruit GFX Library
 * The display setup for TFT_eSPI (pins, ST7796) and CFG_TUH_MIDI are in build_opt.h of this sketch –
 * TFT_eSPI's User_Setup.h stays untouched.
 * IMPORTANT: in Adafruit_TinyUSB_Library/src/arduino/ports/esp32/tusb_config_esp32.h set
 *   CFG_TUH_ENUMERATION_BUFSIZE from 256 to 1024 – the Ampero's configuration descriptor is
 *   505 bytes long, otherwise enumeration fails (see README).
 */
#include <Adafruit_TinyUSB.h>
#include "host/hcd.h"   // HCD_EVENT_* for the enumeration log
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <functional>

#if !defined(CFG_TUH_MIDI) || (CFG_TUH_MIDI == 0)
#error "TinyUSB MIDI host is not enabled (CFG_TUH_MIDI) - build_opt.h missing in the sketch folder?"
#endif
#if !defined(CFG_TUH_MAX3421) || (CFG_TUH_MAX3421 == 0)
#error "TinyUSB is not configured for the MAX3421E host controller."
#endif
#if CFG_TUH_ENUMERATION_BUFSIZE < 512
#error "CFG_TUH_ENUMERATION_BUFSIZE must be >= 512 (Ampero configuration descriptor: 505 bytes). See comment above / README."
#endif
#if !defined(ST7796_DRIVER)
#error "TFT_eSPI is not set up for the ST7796 - build_opt.h missing in the sketch folder?"
#endif

#include "AmperoClient.h"
#include "Config.h"
#include "Controller.h"
#include "Ft6336.h"
#include "Menu.h"
#include "MidiDin.h"
#include "MidiParser.h"
#include "Screen.h"
#include "StripCanvas.h"

// ------------------------------------------------------------------ configuration

// Display ST7796U: SPI2 (FSPI) – the pins are in build_opt.h (MOSI 11, SCLK 12, CS 10, DC 9, RST 8)
constexpr int LCD_ROTATION = 1;             // TFT_eSPI rotation: 1/3 = landscape 480x320, 0/2 = portrait 320x480
constexpr int8_t PIN_LCD_BL = 14;           // backlight (PWM), -1 = backlight wired to the supply
constexpr uint8_t LCD_BRIGHTNESS = 255;     // 0..255

// Touch FT6336U: I2C
constexpr Ft6336::Pins TOUCH_PINS = {/*sda*/ 4, /*scl*/ 5, /*irq*/ 6, /*reset*/ 7};
constexpr bool TOUCH_FLIP_X = false;        // touch mirrored left/right -> true
constexpr bool TOUCH_FLIP_Y = false;        // touch mirrored top/bottom -> true
constexpr uint32_t TILE_HOLD_MS = 1000;     // hold a tile this long -> its settings page

// MAX3421E: SPI3 (HSPI), own bus
constexpr int8_t PIN_MAX_SCK = 39, PIN_MAX_MOSI = 40, PIN_MAX_MISO = 41, PIN_MAX_CS = 42, PIN_MAX_INT = 21;
constexpr int8_t PIN_MAX_RESET = -1;        // RES of the MAX3421E: -1 = wired to 3.3 V

// MIDI-DIN on UART1 (Serial1), EXP ports on ADC1
constexpr uint8_t PIN_MIDI_TX = 17;         // GPIO17 -> MIDI OUT (pin 5 via 10 ohm)
constexpr uint8_t PIN_MIDI_RX = 18;         // GPIO18 <- optocoupler of MIDI IN
constexpr Controller::ExpPins EXP_PINS = {{1, 2}};   // GPIO1 = EXP 1 (tip), GPIO2 = EXP 2 (tip)

constexpr int LCD_W = (LCD_ROTATION & 1) ? 480 : 320;
constexpr int LCD_H = (LCD_ROTATION & 1) ? 320 : 480;
constexpr int STRIP_ROWS = 32;

// ------------------------------------------------------------------ shared between the cores

struct SysExMsg {
  uint16_t len;
  uint8_t data[400];   // Ampero frames are at most 384 bytes; also short MIDI messages
};
static QueueHandle_t rxQueue;   // USB (core 1) -> UI (core 0)
static QueueHandle_t txQueue;   // UI (core 0) -> USB (core 1)
static volatile bool forwardAmperoMidi = false;   // core 1 also queues non-SysEx messages of the Ampero
static volatile int midiIdx = -1;           // TinyUSB MIDI index of the Ampero, -1 = none
static volatile uint32_t usbEpoch = 0;      // changes on every mount/unmount
static volatile uint16_t lastVid = 0, lastPid = 0;
static volatile uint32_t mountCount = 0;
static volatile bool usbStarted = false;
static volatile uint8_t maxRevision = 0;    // MAX3421E REVISION register (0x12/0x13), 0 or 0xFF = no answer
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

static TFT_eSPI tft;
static StripCanvas canvas(LCD_W, LCD_H, STRIP_ROWS);
static AmperoClient ampero;
static Ft6336 touch;
static bool touchOk = false;
static AppConfig cfg;
static MidiDin din;
static Controller ctl;
static Menu menu;

static bool sendFrame(const uint8_t* f, size_t n) {
  if (midiIdx < 0 || n == 0 || n > sizeof(SysExMsg::data)) return false;
  static SysExMsg m;   // static: 400 bytes are too much for every call frame
  m.len = (uint16_t)n;
  memcpy(m.data, f, n);
  return xQueueSend(txQueue, &m, 0) == pdTRUE;
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
    const int rows = canvas.validRows();
    const size_t n = (size_t)LCD_W * rows;
    const uint32_t* w = (const uint32_t*)canvas.buffer();
    uint32_t h = 2166136261u;   // FNV-1a over 32-bit words
    for (size_t i = 0; i < n / 2; i++) h = (h ^ w[i]) * 16777619u;
    if (valid && h == stripHash[s]) continue;
    stripHash[s] = h;
    tft.startWrite();
    tft.setAddrWindow(0, y, LCD_W, rows);
    tft.pushPixels(canvas.buffer(), n);   // setSwapBytes(true): RGB565 is sent high byte first
    tft.endWrite();
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

static void setBacklight(uint8_t level) {
  if (PIN_LCD_BL < 0) return;
  static bool attached = false;
  if (!attached) attached = ledcAttach(PIN_LCD_BL, 5000, 8);
  if (attached) ledcWrite(PIN_LCD_BL, level);
  else digitalWrite(PIN_LCD_BL, level ? HIGH : LOW);
}

// ------------------------------------------------------------------ MIDI-DIN

static void onDinMessage(const uint8_t* m, size_t n) {
  if (cfg.midi.dinThru) din.send(m, n);
  if (cfg.midi.dinToAmpero && midiIdx >= 0) sendFrame(m, n);
}

// ------------------------------------------------------------------ touch

static void handleMenuResult(Menu::Result r) {
  switch (r) {
    case Menu::REDRAW:
      draw();
      break;
    case Menu::CLOSED:
      if (menu.changed()) {
        configSave(cfg);
        applyConfig();
        Serial.println("Settings saved");
      }
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
  static uint32_t downSince = 0, lastPoll = 0, lastRepeat = 0, lastSeen = 0;
  const uint32_t now = millis();
  if (!touchOk || now - lastPoll < 15) return;
  lastPoll = now;

  int rx, ry;
  if (!touch.read(rx, ry)) {
    // released only after two empty reads in a row (the controller reports at ~60 Hz)
    if (down && now - lastSeen > 40) {
      down = false;
      if (pressedTile >= 0) ctl.release(pressedTile);
      pressedTile = -1;
    }
    return;
  }
  lastSeen = now;
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
      Serial.printf("Touch: raw %d/%d -> %d/%d -> tile %d\r\n", rx, ry, x, y, pressedTile + 1);
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

static void uiSetup() {
  if (!canvas.ok()) Serial.println("Error: no RAM for the strip buffer");
  if (!configLoad(cfg)) Serial.println("Settings: defaults");
  applyConfig();

  din.begin(PIN_MIDI_TX, PIN_MIDI_RX);
  ctl.begin(&cfg, &ampero, &din, EXP_PINS);
  menu.begin(&cfg, &ampero, &ctl);

  if (PIN_LCD_BL >= 0) {
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);   // dark until the first frame is on the display
  }
  tft.init();
  tft.setRotation(LCD_ROTATION);
  tft.setSwapBytes(true);
  if (tft.width() != LCD_W || tft.height() != LCD_H)
    Serial.printf("Error: display is %dx%d, expected %dx%d\r\n", tft.width(), tft.height(), LCD_W, LCD_H);
  draw();
  setBacklight(LCD_BRIGHTNESS);

  touchOk = touch.begin(Wire, TOUCH_PINS);
  touch.setMapping(LCD_ROTATION, 320, 480, TOUCH_FLIP_X, TOUCH_FLIP_Y);
  if (touchOk)
    Serial.printf("Touch: FT6336 found (chip 0x%02X, vendor 0x%02X)\r\n", touch.chipId(), touch.vendorId());
  else
    Serial.printf("Error: touch controller does not answer (SDA GPIO%d, SCL GPIO%d)\r\n", TOUCH_PINS.sda,
                  TOUCH_PINS.scl);

  ampero.begin(sendFrame);
}

/** Logs the MAX3421E check and every enumeration step (attach, descriptors, mount). */
static void usbDiagnostics() {
  static uint32_t lastPoll = 0;
  static uint32_t shownAttach = 0, shownRemove = 0, shownDev = 0, shownCfg = 0;
  static bool startShown = false;
  const uint32_t now = millis();
  if (now - lastPoll < 100) return;
  lastPoll = now;

  if (usbStarted && !startShown) {
    startShown = true;
    const uint8_t rev = maxRevision;
    if (rev == 0x12 || rev == 0x13)
      Serial.printf("USB host started: MAX3421E revision 0x%02X\r\n", rev);
    else
      Serial.printf("Error: MAX3421E does not answer (REVISION = 0x%02X) - check SPI wiring "
                    "(SCK %d, MOSI %d, MISO %d, CS %d), INT %d, RES and 3.3 V\r\n",
                    rev, PIN_MAX_SCK, PIN_MAX_MOSI, PIN_MAX_MISO, PIN_MAX_CS, PIN_MAX_INT);
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

static void uiLoop() {
  static uint32_t epoch = 0, drawnRevision = 0xFFFFFFFF, drawnCtl = 0xFFFFFFFF, lastDraw = 0, shownMounts = 0;
  static bool drawnConnected = false;
  static SysExMsg m;

  usbDiagnostics();

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
  while (xQueueReceive(rxQueue, &m, 0) == pdTRUE) {
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

static void uiTask(void*) {
  uiSetup();
  for (;;) {
    uiLoop();
    vTaskDelay(1);   // 1 ms: lets the idle task of core 0 run (task watchdog)
  }
}

// ------------------------------------------------------------------ core 1: USB host

static SPIClass maxSpi(HSPI);   // SPI3, the display uses SPI2
Adafruit_USBH_Host USBHost(&maxSpi, PIN_MAX_SCK, PIN_MAX_MOSI, PIN_MAX_MISO, PIN_MAX_CS, PIN_MAX_INT);

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
  if (xQueueSend(rxQueue, &m, 0) != pdTRUE) midiRxDropped = midiRxDropped + 1;   // UI task is behind
}

void setup() {
  Serial.begin(115200);
  rxQueue = xQueueCreate(32, sizeof(SysExMsg));
  txQueue = xQueueCreate(16, sizeof(SysExMsg));

  // display, touch, protocol: own task on core 0
  xTaskCreatePinnedToCore(uiTask, "ui", 16384, nullptr, 1, nullptr, 0);

  // USB host: here on core 1 (the library's MAX3421 interrupt task runs on this core, too)
  if (PIN_MAX_RESET >= 0) {
    pinMode(PIN_MAX_RESET, OUTPUT);
    digitalWrite(PIN_MAX_RESET, LOW);
    delay(10);
    digitalWrite(PIN_MAX_RESET, HIGH);
    delay(10);
  }
  USBHost.begin(1);
  maxRevision = USBHost.max3421_readRegister(18 << 3, false);   // REVISION
  usbStarted = true;
}

// The Ampero stops accepting OUT data after a zero-length packet, and TinyUSB sends one after every
// transfer of exactly 64 bytes (the editor never does). So messages are packed into USB-MIDI packets
// here and sent in transfers of at most 15 packets (60 bytes), one at a time.
static volatile bool txInFlight = false;
constexpr int kTxChunkPackets = 15;
constexpr uint32_t kTxTimeoutMs = 500;

void loop() {
  static SysExMsg tx;
  static size_t txOff = 0;
  static bool txBusy = false;
  static uint32_t txSince = 0;

  USBHost.task(1);   // waits up to 1 ms for USB events

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
  if (!txBusy && xQueueReceive(txQueue, &tx, 0) == pdTRUE) {
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
