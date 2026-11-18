#pragma once
/*
 * Effect categories of the Ampero II (module index in preset tag 4, "Catalog" index in the editor's
 * algorithm database) with the colours the editor uses for them (effectHighLightColor).
 */
#include <stdint.h>

constexpr int kCategoryCount = 17;

inline const char* categoryName(int c) {
  static const char* const kNames[kCategoryCount] = {"DYN", "FREQ", "WAH",    "DRV",    "AMP",     "PRE AMP",
                                                     "CAB", "IR",   "EQ",     "MOD",    "DLY",     "RVB",
                                                     "FX SND", "FX RTN", "FX LOOP", "VOL", "CATCH"};
  return c >= 0 && c < kCategoryCount ? kNames[c] : "";
}

constexpr uint16_t rgb565c(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline uint16_t categoryColor(int c) {
  static const uint16_t kColors[kCategoryCount] = {
      rgb565c(42, 189, 252),  // DYN
      rgb565c(83, 86, 252),   // FREQ
      rgb565c(155, 88, 226),  // WAH
      rgb565c(254, 85, 87),   // DRV
      rgb565c(89, 155, 252),  // AMP
      rgb565c(177, 255, 75),  // PRE AMP
      rgb565c(243, 184, 93),  // CAB
      rgb565c(229, 229, 229), // IR
      rgb565c(86, 222, 218),  // EQ
      rgb565c(71, 234, 97),   // MOD
      rgb565c(237, 74, 148),  // DLY
      rgb565c(237, 217, 82),  // RVB
      rgb565c(231, 124, 53),  // FX SND
      rgb565c(231, 124, 53),  // FX RTN
      rgb565c(231, 124, 53),  // FX LOOP
      rgb565c(194, 76, 230),  // VOL
      rgb565c(170, 170, 170), // CATCH
  };
  return c >= 0 && c < kCategoryCount ? kColors[c] : rgb565c(150, 156, 168);
}
