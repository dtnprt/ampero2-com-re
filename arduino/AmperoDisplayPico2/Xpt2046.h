#pragma once
/*
 * XPT2046 / ADS7843 resistive touch controller of the HY28B (own pins TP_*), on its own SPI bus.
 * Raw readings are mapped to screen coordinates with a 3-point affine calibration, which also
 * covers swapped/mirrored axes, and stored in the flash-emulated EEPROM.
 */
#include <Arduino.h>
#include <SPI.h>

struct TouchCalibration {
  // screen_x = a*rx + b*ry + c ; screen_y = d*rx + e*ry + f
  float a, b, c, d, e, f;
  uint32_t magic;
  static constexpr uint32_t kMagic = 0x41545031;   // "ATP1"
  bool valid() const { return magic == kMagic; }
};

class Xpt2046 {
 public:
  /** Size of the flash-emulated EEPROM (calibration at 0, app settings at 64; see Config.cpp). */
  static constexpr int kEepromSize = 1024;

  struct Pins {
    uint8_t sck, mosi, miso, cs, irq;
  };

  void begin(SPIClassRP2040& spi, const Pins& pins);
  /** Pen-down according to the IRQ line (cheap, no SPI traffic). */
  bool penDown() const { return digitalRead(p_.irq) == LOW; }
  /** Averaged raw sample; false if the pressure is too low (not really touched). */
  bool readRaw(int& rx, int& ry);

  void setCalibration(const TouchCalibration& c) { cal_ = c; }
  const TouchCalibration& calibration() const { return cal_; }
  /** Raw -> screen coordinates (requires a valid calibration). */
  void map(int rx, int ry, int& sx, int& sy) const;

  /** Computes the calibration from three screen points and their raw readings. */
  static bool solve(const int sx[3], const int sy[3], const int rx[3], const int ry[3], TouchCalibration& out);

  static bool loadCalibration(TouchCalibration& c);
  static void saveCalibration(const TouchCalibration& c);

 private:
  uint16_t measure(uint8_t cmd);
  SPIClassRP2040* spi_ = nullptr;
  Pins p_{};
  TouchCalibration cal_{};
};
