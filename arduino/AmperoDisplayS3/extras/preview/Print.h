#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* b, size_t n) { size_t r = 0; while (n--) r += write(*b++); return r; }
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
};
