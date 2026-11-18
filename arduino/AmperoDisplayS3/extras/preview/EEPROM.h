#pragma once
#include <cstring>
struct EEPROMShim {
  unsigned char mem[1024];
  void begin(int) {}
  template <class T> void get(int a, T& t) { memcpy(&t, mem + a, sizeof t); }
  template <class T> void put(int a, const T& t) { memcpy(mem + a, &t, sizeof t); }
  bool commit() { return true; }
};
inline EEPROMShim EEPROM;
