#pragma once
/*
 * FT6336U capacitive touch controller (I2C, address 0x38), first touch point only.
 * Raw coordinates are in the panel's native portrait frame (320 x 480); map() turns them into
 * screen coordinates for the TFT_eSPI rotation (0..3). No calibration needed.
 */
#include <Arduino.h>
#include <Wire.h>

class Ft6336 {
 public:
  struct Pins {
    int8_t sda, scl, irq, reset;   // irq/reset: -1 = not connected
  };

  /** Returns false if the controller does not answer. */
  bool begin(TwoWire& wire, const Pins& pins, uint32_t i2cHz = 400000);
  /** Reads the first touch point (raw panel coordinates); false if not touched or no answer. */
  bool read(int& rawX, int& rawY);
  /** Sets the mapping: screen rotation 0..3 (as TFT_eSPI), native size, optional mirroring after rotation. */
  void setMapping(int rotation, int nativeW, int nativeH, bool flipX, bool flipY);
  /** Raw -> screen coordinates. */
  void map(int rawX, int rawY, int& x, int& y) const;

  uint8_t chipId() const { return chipId_; }
  uint8_t vendorId() const { return vendorId_; }

 private:
  bool readRegs(uint8_t reg, uint8_t* buf, size_t n);
  void writeReg(uint8_t reg, uint8_t val);

  TwoWire* wire_ = nullptr;
  Pins p_{};
  uint8_t chipId_ = 0, vendorId_ = 0;
  int rot_ = 1, nw_ = 320, nh_ = 480;
  bool flipX_ = false, flipY_ = false;
};
