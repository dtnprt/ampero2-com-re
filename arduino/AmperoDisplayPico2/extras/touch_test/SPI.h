#pragma once
#include <cstdint>
#define MSBFIRST 1
#define SPI_MODE0 0
struct SPISettings { SPISettings(uint32_t, int, int) {} };
class SPIClassRP2040 {
 public:
  void setSCK(int) {} void setTX(int) {} void setRX(int) {} void begin() {}
  void beginTransaction(const SPISettings&) {} void endTransaction() {}
  uint8_t transfer(uint8_t) { return 0; } uint16_t transfer16(uint16_t) { return 0; }
};
