#pragma once
/*
 * Settings made on the touchscreen, stored in flash (EEPROM emulation in NVS).
 * Buttons 0..5 = the six tiles, 6/7 = footswitches on EXP port 1/2.
 */
#include <stdint.h>

enum ButtonMode : uint8_t {
  BTN_OFF = 0,
  BTN_SCENE,         // a = scene 0..2
  BTN_SCENE_TOGGLE,  // a, b = scenes; each press switches between them
  BTN_BLOCK,         // a = target: 0..11 slot, BLOCK_CATEGORY + c = first slot of category c
  BTN_DEVICE_FS,     // fsMask bit 0..2 = do what device footswitch 1..3 does
  BTN_MODE_COUNT
};
constexpr uint8_t BLOCK_CATEGORY = 16;

enum MidiType : uint8_t { MIDI_OFF = 0, MIDI_CC, MIDI_PC, MIDI_NOTE, MIDI_TYPE_COUNT };

/** Additional standard MIDI message on MIDI-DIN out. */
struct MidiAction {
  uint8_t type;       // MidiType
  uint8_t channel;    // 0..15
  uint8_t number;     // CC number / program / note
  uint8_t valueOn;    // CC value on 1st, 3rd ... press / note velocity
  uint8_t valueOff;   // CC value on 2nd, 4th ... press (= valueOn: always the same value)
};

struct ButtonConfig {
  uint8_t mode;       // ButtonMode
  uint8_t a, b;
  uint8_t fsMask;
  MidiAction midi;
  uint8_t reserved[3];
};

enum ExpMode : uint8_t { EXP_OFF = 0, EXP_PEDAL, EXP_SWITCH, EXP_MODE_COUNT };
enum PedalTarget : uint8_t {
  PEDAL_AMPERO_EXP1 = 0,  // CC 13 = 0, CC 11 = value
  PEDAL_AMPERO_EXP2,      // CC 13 = 127, CC 11 = value
  PEDAL_PATCH_VOLUME,     // CC 7 = 0..100
  PEDAL_QUICK1,           // CC 16
  PEDAL_QUICK2,           // CC 18
  PEDAL_QUICK3,           // CC 20
  PEDAL_DIN_CC,           // channel/cc on MIDI-DIN
  PEDAL_TARGET_COUNT
};

struct ExpConfig {
  uint8_t mode;       // ExpMode
  uint8_t target;     // PedalTarget
  uint8_t channel;    // 0..15 (PEDAL_DIN_CC)
  uint8_t cc;         // (PEDAL_DIN_CC)
  uint16_t rawMin, rawMax;   // ADC values at heel / toe (12 bit)
  uint8_t latching;   // footswitch: 0 = momentary, 1 = latching (every change is a press)
  uint8_t reserved[3];
};

struct MidiRouting {
  uint8_t amperoChannel;   // 1..16: channel of the CC commands to the Ampero
  uint8_t dinToAmpero;     // MIDI-DIN in -> Ampero (USB)
  uint8_t dinThru;         // MIDI-DIN in -> MIDI-DIN out
  uint8_t amperoToDin;     // Ampero (USB, except its SysEx protocol) -> MIDI-DIN out
};

struct AppConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  ButtonConfig buttons[8];
  ExpConfig exp[2];
  MidiRouting midi;
  uint32_t check;

  static constexpr uint32_t kMagic = 0x41435347;   // "GSCA"
  static constexpr uint16_t kVersion = 1;
};

void configDefaults(AppConfig& c);
/** Loads the settings; defaults if nothing (valid) is stored. */
bool configLoad(AppConfig& c);
void configSave(AppConfig& c);
