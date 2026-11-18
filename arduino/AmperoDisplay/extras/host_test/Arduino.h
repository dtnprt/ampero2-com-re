// Minimal Arduino shim to compile AmperoClient.cpp on a PC (host test only).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }

struct SerialShim {
  template <class... A>
  void printf(const char* f, A... a) { std::printf(f, a...); }
};
extern SerialShim Serial0;
