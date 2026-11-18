// Minimal shim for the PC test of Xpt2046 calibration math and Screen hit testing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#define PROGMEM
#define LOW 0
#define HIGH 1
#define OUTPUT 1
#define INPUT_PULLUP 2
typedef bool boolean;
typedef uint8_t byte;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }
#include "Print.h"
class __FlashStringHelper;
class String { public: const char* c_str() const { return ""; } unsigned length() const { return 0; } };
inline double radians(double d) { return d * 3.14159265358979323846 / 180.0; }
inline uint16_t __builtin_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
