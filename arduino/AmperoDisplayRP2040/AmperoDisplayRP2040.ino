/*
 * AmperoDisplayRP2040 – zeigt Preset-Name, die drei Szenen-Namen und die aktive Szene eines
 * Hotone Ampero II (Stomp) auf einem HY28B (2.8", ILI9325, SPI) an einem Waveshare RP2040-Zero.
 *
 * Core 1: PIO-USB-Host (Adafruit TinyUSB + Pico-PIO-USB, eingebauter TinyUSB-MIDI-Host)
 * Core 0: Ampero-Protokoll (AmperoClient) und Display; Austausch über zwei Queues.
 * Der native USB-C-Port bleibt für Programmierung und Serial-Log (115200) frei.
 *
 * Arduino-IDE:
 *   Board:      Raspberry Pi Pico/RP2040 (earlephilhower) -> "Waveshare RP2040 Zero"
 *   CPU Speed:  120 MHz            (PIO-USB braucht 120 oder 240 MHz)
 *   USB Stack:  Adafruit TinyUSB
 * Bibliotheken: Adafruit TinyUSB Library, Pico PIO USB, Adafruit GFX Library
 * WICHTIG: In Adafruit_TinyUSB_Library/src/arduino/ports/rp2040/tusb_config_rp2040.h
 *   CFG_TUH_ENUMERATION_BUFSIZE von 256 auf 1024 setzen – der Konfigurationsdeskriptor des
 *   Ampero ist 505 Byte lang, sonst bricht die Enumeration ab (siehe README).
 */
#include "pio_usb.h"
#include <Adafruit_TinyUSB.h>
#include <hardware/clocks.h>
#include <pico/util/queue.h>

#if !defined(CFG_TUH_MIDI) || (CFG_TUH_MIDI == 0)
#error "TinyUSB-MIDI-Host ist nicht aktiviert (CFG_TUH_MIDI) - USB Stack 'Adafruit TinyUSB' wählen."
#endif
#if CFG_TUH_ENUMERATION_BUFSIZE < 512
#error "CFG_TUH_ENUMERATION_BUFSIZE muss >= 512 sein (Ampero-Konfigurationsdeskriptor: 505 Byte). Siehe Kommentar oben / README."
#endif

#include "AmperoClient.h"
#include "Ili9325Spi.h"
#include "Screen.h"
#include "StripCanvas.h"

// ------------------------------------------------------------------ configuration

// USB host (PIO): D- is always D+ + 1
constexpr uint8_t PIN_USB_DP = 14;   // GP14 = D+, GP15 = D-

// HY28B (SPI0)
constexpr Ili9325Spi::Pins LCD_PINS = {
    /*sck*/ 2, /*mosi*/ 3, /*miso*/ 4, /*cs*/ 5, /*reset*/ 6, /*backlight*/ 7};
constexpr int LCD_ROTATION = 90;            // 90/270 = quer (320x240), 0/180 = hoch (240x320)
constexpr uint32_t LCD_SPI_HZ = 16000000;   // ILI9325 nominal 10 MHz; fbtft runs the HY28B at up to 48 MHz
constexpr uint8_t LCD_BRIGHTNESS = 255;     // 0..255 (PWM on BL_CTRL)

constexpr int LCD_W = (LCD_ROTATION == 90 || LCD_ROTATION == 270) ? 320 : 240;
constexpr int LCD_H = (LCD_ROTATION == 90 || LCD_ROTATION == 270) ? 240 : 320;
constexpr int STRIP_ROWS = 30;

// ------------------------------------------------------------------ shared between the cores

struct SysExMsg {
  uint16_t len;
  uint8_t data[400];   // Ampero frames are at most 384 bytes
};
static queue_t rxQueue;   // core1 -> core0
static queue_t txQueue;   // core0 -> core1
static volatile bool queuesReady = false;
static volatile int midiIdx = -1;           // TinyUSB MIDI index of the Ampero, -1 = none
static volatile uint32_t usbEpoch = 0;      // changes on every mount/unmount
static volatile uint16_t lastVid = 0, lastPid = 0;
static volatile uint32_t mountCount = 0;
static volatile bool clockError = false;

// ------------------------------------------------------------------ core 0: protocol + display

static Ili9325Spi lcd;
static StripCanvas canvas(LCD_W, LCD_H, STRIP_ROWS);
static AmperoClient ampero;

static bool sendFrame(const uint8_t* f, size_t n) {
  if (midiIdx < 0 || n > sizeof(SysExMsg::data)) return false;
  static SysExMsg m;   // static: 400 bytes are too much for every call frame
  m.len = (uint16_t)n;
  memcpy(m.data, f, n);
  return queue_try_add(&txQueue, &m);
}

static void draw() {
  ScreenState s;
  s.usbConnected = midiIdx >= 0;
  s.online = ampero.online();
  s.hasPreset = ampero.hasPreset();
  s.presetIndex = ampero.presetIndex();
  s.presetName = ampero.presetName();
  for (int i = 0; i < 3; i++) s.sceneNames[i] = ampero.sceneName(i);
  s.activeScene = ampero.activeScene();

  lcd.beginFrame();
  for (int y = 0; y < LCD_H; y += STRIP_ROWS) {
    canvas.setStrip(y);
    renderScreen(canvas, s);
    lcd.writePixels(canvas.buffer(), (size_t)LCD_W * canvas.validRows());
  }
}

void setup() {
  queue_init(&rxQueue, sizeof(SysExMsg), 8);
  queue_init(&txQueue, sizeof(SysExMsg), 4);
  queuesReady = true;

  Serial.begin(115200);
  if (!canvas.ok()) Serial.println("Fehler: kein RAM für den Streifenpuffer");
  lcd.begin(LCD_PINS, LCD_ROTATION, LCD_SPI_HZ);
  draw();
  lcd.setBacklight(LCD_BRIGHTNESS);
  ampero.begin(sendFrame);
}

void loop() {
  static uint32_t epoch = 0, drawnRevision = 0xFFFFFFFF, lastDraw = 0, shownMounts = 0;
  static bool drawnConnected = false, clockReported = false;
  static SysExMsg m;

  if (clockError && !clockReported) {
    clockReported = true;
    Serial.printf("Fehler: CPU-Takt %lu Hz - Tools -> CPU Speed auf 120 oder 240 MHz stellen\r\n",
                  (unsigned long)clock_get_hz(clk_sys));
  }
  if (mountCount != shownMounts) {
    shownMounts = mountCount;
    Serial.printf("USB: Gerät %04x:%04x angemeldet\r\n", lastVid, lastPid);
  }
  if (usbEpoch != epoch) {
    epoch = usbEpoch;
    if (midiIdx >= 0) {
      Serial.printf("USB-MIDI verbunden (Index %d)\r\n", midiIdx);
      ampero.onConnected();
    } else {
      Serial.println("USB-MIDI getrennt");
      ampero.onDisconnected();
    }
  }

  while (queue_try_remove(&rxQueue, &m)) ampero.onSysEx(m.data, m.len);
  ampero.loop();

  const bool connected = midiIdx >= 0;
  const uint32_t now = millis();
  if ((ampero.revision() != drawnRevision || connected != drawnConnected) && now - lastDraw > 30) {
    drawnRevision = ampero.revision();
    drawnConnected = connected;
    lastDraw = now;
    draw();
  }
}

// ------------------------------------------------------------------ core 1: USB host

Adafruit_USBH_Host USBHost;

static uint8_t sx[400];
static size_t sxLen = 0;
static bool sxActive = false;

static void pushByte(uint8_t b) {
  if (b >= 0xF8) return;                       // realtime
  if (b == 0xF0) { sxActive = true; sxLen = 0; }
  if (!sxActive) return;
  if (sxLen >= sizeof sx) { sxActive = false; return; }   // too long -> drop
  sx[sxLen++] = b;
  if (b == 0xF7) {
    sxActive = false;
    static SysExMsg m;
    m.len = (uint16_t)sxLen;
    memcpy(m.data, sx, sxLen);
    queue_try_add(&rxQueue, &m);               // dropped if core 0 is behind
  } else if (b & 0x80 && b != 0xF0) {
    sxActive = false;                          // other status byte aborts SysEx
  }
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
  USBHost.configure_pio_usb(1, &cfg);
  USBHost.begin(1);
}

void loop1() {
  static SysExMsg tx;
  static size_t txOff = 0;
  static bool txBusy = false;

  USBHost.task();

  const int idx = midiIdx;
  if (idx < 0) {
    txBusy = false;
    return;
  }
  if (!txBusy && queue_try_remove(&txQueue, &tx)) {
    txBusy = true;
    txOff = 0;
  }
  if (txBusy) {
    txOff += tuh_midi_stream_write((uint8_t)idx, 0, tx.data + txOff, tx.len - txOff);
    tuh_midi_write_flush((uint8_t)idx);
    if (txOff >= tx.len) txBusy = false;
  }
}

// TinyUSB host callbacks (run on core 1 inside USBHost.task())
extern "C" {

void tuh_mount_cb(uint8_t dev_addr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  lastVid = vid;
  lastPid = pid;
  mountCount = mountCount + 1;
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* mount_cb_data) {
  (void)mount_cb_data;
  sxActive = false;
  midiIdx = idx;
  usbEpoch = usbEpoch + 1;
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
  while ((n = tuh_midi_stream_read(idx, &cable, buf, sizeof buf)) > 0)
    for (uint32_t i = 0; i < n; i++) pushByte(buf[i]);
}

}  // extern "C"
