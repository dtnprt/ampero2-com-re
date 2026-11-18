/*
 * AmperoDisplay – zeigt den aktiven Preset-Namen, die drei Szenen-Namen und die aktive Szene
 * eines Hotone Ampero II (Stomp) auf dem LCDwiki 3.5" ESP32-S3 Display (ES3C35P, ST77922, 320x480).
 *
 * Der ESP32-S3 ist USB-Host, der Ampero hängt per USB (OTG) daran. Beim Verbinden meldet sich der
 * Sketch wie der Editor an (Befehl 0x05000017) und fragt danach jede Sekunde Preset und Szene ab;
 * Szenenwechsel am Gerät werden zusätzlich als Meldung empfangen.
 *
 * Arduino-IDE (Core "esp32" by Espressif >= 3.1, kompiliert mit 3.3.12):
 *   Board:            ESP32S3 Dev Module
 *   Flash Size:       16MB          PSRAM: OPI PSRAM
 *   USB Mode:         USB-OTG (TinyUSB)
 *   USB CDC On Boot:  Disabled      (Debug-Ausgabe über UART0 = IO43/IO44, 115200 Baud)
 *   Upload Mode:      UART0 / Hardware CDC  (zum Flashen BOOT halten + RESET)
 * Bibliothek: Adafruit GFX Library (>= 1.11)
 */
#include <Adafruit_GFX.h>

#include "AmperoClient.h"
#include "Screen.h"
#include "St77922Qspi.h"
#include "UsbMidiHost.h"

// 1 or 3 = landscape (480x320), 0 or 2 = portrait (320x480)
#define SCREEN_ROTATION 1

/** GFX canvas whose 300 KB framebuffer lives in PSRAM. */
class PsramCanvas : public GFXcanvas16 {
 public:
  PsramCanvas(uint16_t w, uint16_t h) : GFXcanvas16(w, h, false) {
    buffer = (uint16_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    buffer_owned = false;
  }
  bool ok() const { return buffer != nullptr; }
};

static St77922Qspi lcd;
static UsbMidiHost usb;
static AmperoClient ampero;
static PsramCanvas canvas(St77922Qspi::kWidth, St77922Qspi::kHeight);

static bool sendFrame(const uint8_t* f, size_t n) { return usb.sendSysEx(f, n); }

static void draw() {
  ScreenState s;
  s.usbConnected = usb.connected();
  s.online = ampero.online();
  s.hasPreset = ampero.hasPreset();
  s.presetIndex = ampero.presetIndex();
  s.presetName = ampero.presetName();
  for (int i = 0; i < 3; i++) s.sceneNames[i] = ampero.sceneName(i);
  s.activeScene = ampero.activeScene();
  renderScreen(canvas, s);
  lcd.pushFrame(canvas.getBuffer());
}

void setup() {
  Serial0.begin(115200);
  Serial0.println("\r\nAmperoDisplay startet");

  if (!canvas.ok()) Serial0.println("Fehler: kein PSRAM für den Framebuffer (Tools -> PSRAM: OPI PSRAM)");
  if (!lcd.begin()) Serial0.println("Fehler: Display-Initialisierung");
  canvas.setRotation(SCREEN_ROTATION);
  draw();
  lcd.setBacklight(true);

  ampero.begin(sendFrame);
  if (!usb.begin()) Serial0.println("Fehler: USB-Host konnte nicht gestartet werden");
}

void loop() {
  static uint32_t epoch = 0, drawnRevision = 0xFFFFFFFF, lastDraw = 0;
  static bool drawnConnected = false;
  static uint8_t msg[512];

  if (usb.connectionEpoch() != epoch) {
    epoch = usb.connectionEpoch();
    if (usb.connected()) {
      Serial0.printf("USB-MIDI verbunden (%04x:%04x)\r\n", usb.vid(), usb.pid());
      ampero.onConnected();
    } else {
      ampero.onDisconnected();
    }
  }

  size_t n;
  while ((n = usb.readSysEx(msg, sizeof msg)) > 0) ampero.onSysEx(msg, n);
  ampero.loop();

  const uint32_t now = millis();
  if ((ampero.revision() != drawnRevision || usb.connected() != drawnConnected) && now - lastDraw > 30) {
    drawnRevision = ampero.revision();
    drawnConnected = usb.connected();
    lastDraw = now;
    draw();
  }
  delay(2);
}
