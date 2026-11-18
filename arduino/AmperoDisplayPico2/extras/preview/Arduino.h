// Minimal Arduino shim to compile Adafruit GFX + Screen.cpp on a PC (preview only).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define PROGMEM
typedef bool boolean;
typedef uint8_t byte;
#include "Print.h"
class __FlashStringHelper;
class String {
 public:
  const char* c_str() const { return ""; }
  unsigned length() const { return 0; }
};
inline double radians(double d) { return d * 3.14159265358979323846 / 180.0; }
inline uint32_t millis() { return 0; }
struct SerialStub {
  template <class... A>
  void printf(const char* f, A... a) { ::printf(f, a...); }
};
inline SerialStub Serial0;
inline void delayMicroseconds(unsigned) {}
#define LOW 0
#define HIGH 1
#define OUTPUT 1
#define INPUT_PULLUP 2
inline int digitalRead(int) { return HIGH; }
inline void digitalWrite(int, int) {}
inline void pinMode(int, int) {}
struct UartStub {
  void setTX(int) {} void setRX(int) {} void setFIFOSize(int) {} void begin(int) {}
  size_t write(const uint8_t*, size_t n) { return n; }
  int available() { return 0; } int read() { return -1; }
};
inline UartStub Serial1;
inline uint16_t __builtin_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
