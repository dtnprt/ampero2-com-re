#pragma once
/*
 * Minimal ST77922 QSPI driver for the LCDwiki 3.5" ESP32-S3 display (ES3C35P, 320x480).
 * Uses ESP-IDF esp_lcd (QSPI panel IO) from the arduino-esp32 core 3.x.
 * The init sequence was extracted from the vendor's demo firmware (lv_demo_widgets.bin).
 */
#include <Arduino.h>

class St77922Qspi {
 public:
  static constexpr int kWidth = 320;   // native orientation (portrait)
  static constexpr int kHeight = 480;

  bool begin();
  void setBacklight(bool on);
  /** Sends a full 320x480 RGB565 frame (native little-endian uint16, row-major, portrait). */
  void pushFrame(const uint16_t* pixels);

 private:
  void writeCommand(uint8_t cmd, const uint8_t* data, size_t len);
  void writePixels(int y0, int rows, const uint16_t* src);
};
