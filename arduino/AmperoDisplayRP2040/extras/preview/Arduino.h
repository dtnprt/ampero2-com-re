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
inline uint16_t __builtin_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
