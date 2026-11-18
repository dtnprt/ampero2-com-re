#include "Config.h"

#include <EEPROM.h>
#include <stddef.h>
#include <string.h>


namespace {

constexpr int kOffset = 64;
constexpr int kEepromSize = 1024;

uint32_t checksum(const AppConfig& c) {
  const uint8_t* p = (const uint8_t*)&c;
  uint32_t h = 2166136261u;   // FNV-1a over everything before 'check'
  for (size_t i = 0; i < offsetof(AppConfig, check); i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

}  // namespace

void configDefaults(AppConfig& c) {
  memset(&c, 0, sizeof c);
  c.magic = AppConfig::kMagic;
  c.version = AppConfig::kVersion;
  c.size = sizeof c;
  for (int i = 0; i < 3; i++) {   // top row: scene 1..3, bottom row: device footswitch 1..3
    c.buttons[i].mode = BTN_SCENE;
    c.buttons[i].a = (uint8_t)i;
    c.buttons[3 + i].mode = BTN_DEVICE_FS;
    c.buttons[3 + i].fsMask = (uint8_t)(1 << i);
  }
  for (int i = 0; i < 8; i++) {
    c.buttons[i].midi.number = (uint8_t)(80 + i);
    c.buttons[i].midi.valueOn = 127;
    c.buttons[i].midi.valueOff = 0;
  }
  for (int p = 0; p < 2; p++) {
    c.exp[p].target = (uint8_t)(PEDAL_AMPERO_EXP1 + p);
    c.exp[p].cc = (uint8_t)(11 + p);
    c.exp[p].rawMin = 0;
    c.exp[p].rawMax = 4095;
  }
  c.midi.amperoChannel = 1;
  c.midi.dinToAmpero = 1;
  c.midi.dinThru = 0;
  c.midi.amperoToDin = 0;
  c.check = checksum(c);
}

bool configLoad(AppConfig& c) {
  EEPROM.begin(kEepromSize);
  EEPROM.get(kOffset, c);
  if (c.magic == AppConfig::kMagic && c.version == AppConfig::kVersion && c.size == sizeof c && c.check == checksum(c))
    return true;
  configDefaults(c);
  return false;
}

void configSave(AppConfig& c) {
  c.magic = AppConfig::kMagic;
  c.version = AppConfig::kVersion;
  c.size = sizeof c;
  c.check = checksum(c);
  EEPROM.begin(kEepromSize);
  EEPROM.put(kOffset, c);
  EEPROM.commit();
}
