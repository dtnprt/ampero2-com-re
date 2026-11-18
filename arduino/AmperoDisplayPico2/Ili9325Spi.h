#pragma once
/*
 * ILI9325 over SPI (HY28B 2.8" 240x320 module, IM jumpers = SPI, the factory default).
 * ILI9325 serial protocol: every CS cycle starts with a start byte
 *   0x70 = write index register, 0x72 = write data (register value or GRAM pixels).
 * Init sequence, rotation and gamma follow the Linux fbtft "hy28b" profile
 * (fbtft_device.c / fb_ili9325.c): SPI mode 3, BGR panel.
 */
#include <Arduino.h>
#include <SPI.h>

class Ili9325Spi {
 public:
  struct Pins {
    uint8_t sck, mosi, miso, cs, reset, backlight;
  };

  /** rotation: 0/180 = portrait 240x320, 90/270 = landscape 320x240 */
  bool begin(const Pins& pins, int rotation, uint32_t spiHz);
  int width() const { return (rot_ == 90 || rot_ == 270) ? 320 : 240; }
  int height() const { return (rot_ == 90 || rot_ == 270) ? 240 : 320; }
  void setBacklight(uint8_t level);   // 0..255

  /** Starts a full-screen write; pixels follow row by row in logical (rotated) order. */
  void beginFrame() { beginFrameAt(0); }
  /** Starts a write at logical row y (full width); pixels follow row by row until the end of the screen. */
  void beginFrameAt(int y);
  /** Writes n RGB565 pixels; the buffer is byte-swapped in place (destroyed). */
  void writePixels(uint16_t* px, size_t n);

 private:
  void writeReg(uint16_t reg, uint16_t val);
  void writeIndex(uint16_t reg);
  Pins p_{};
  int rot_ = 90;
  SPISettings spi_;
};
