#pragma once
/*
 * Adafruit-GFX target that only keeps a horizontal strip of the screen in RAM.
 * The full screen is drawn once per strip (setStrip), drawing outside the strip is clipped.
 * Lets a 320x240 RGB565 UI run with ~19 KB instead of a 150 KB framebuffer.
 */
#include <Adafruit_GFX.h>
#include <stdlib.h>

class StripCanvas : public Adafruit_GFX {
 public:
  StripCanvas(int16_t w, int16_t h, int16_t stripRows)
      : Adafruit_GFX(w, h), rows_(stripRows), buf_((uint16_t*)malloc((size_t)w * stripRows * 2)) {}

  bool ok() const { return buf_ != nullptr; }
  void setStrip(int16_t y0) { y0_ = y0; }
  int16_t stripY() const { return y0_; }
  int16_t stripRows() const { return rows_; }
  /** Rows of the current strip that are on screen. */
  int16_t validRows() const { return (y0_ + rows_ <= HEIGHT) ? rows_ : (int16_t)(HEIGHT - y0_); }
  uint16_t* buffer() { return buf_; }

  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || x >= WIDTH || y < y0_ || y >= y0_ + rows_) return;
    buf_[(y - y0_) * WIDTH + x] = c;
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    int16_t x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < y0_) y = y0_;
    if (x1 > WIDTH) x1 = WIDTH;
    if (y1 > y0_ + rows_) y1 = y0_ + rows_;
    for (int16_t yy = y; yy < y1; yy++) {
      uint16_t* p = buf_ + (yy - y0_) * WIDTH;
      for (int16_t xx = x; xx < x1; xx++) p[xx] = c;
    }
  }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override { fillRect(x, y, w, 1, c); }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override { fillRect(x, y, 1, h, c); }
  void fillScreen(uint16_t c) override { fillRect(0, 0, WIDTH, HEIGHT, c); }

 private:
  int16_t rows_;
  int16_t y0_ = 0;
  uint16_t* buf_;
};
