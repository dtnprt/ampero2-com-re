#pragma once
/*
 * What the six touch footswitches (tiles) and the two expression ports do, according to AppConfig:
 * executes presses, reads the EXP ports (pedal or footswitch) and builds the tile contents.
 */
#include <Arduino.h>

#include "AmperoClient.h"
#include "Config.h"
#include "MidiDin.h"
#include "Screen.h"

class Controller {
 public:
  struct ExpPins {
    uint8_t pin[2];   // ADC1-capable GPIOs (1..10)
  };

  void begin(AppConfig* cfg, AmperoClient* ampero, MidiDin* din, const ExpPins& pins);
  /** Button 0..5 = tile, 6/7 = footswitch on EXP port 1/2. */
  void press(int button);
  void release(int button);
  /** Contents of tile i (0..5). */
  void tileView(int i, TileView& v) const;
  /** Reads the EXP ports; call often. */
  void poll();
  /** Re-reads the EXP port modes after a configuration change. */
  void applyExpConfig();

  /** Filtered ADC value 0..4095 of EXP port p (for the calibration page), -1 if not a pedal. */
  int expRaw(int p) const;
  /** Last sent pedal value 0..127, -1 if none. */
  int expValue(int p) const { return expSent_[p]; }
  /** Increments when something on the main screen may have changed (button pressed, EXP mode). */
  uint32_t revision() const { return revision_; }
  /** Increments with every new pedal value (calibration page). */
  uint32_t expRevision() const { return expRevision_; }

  /** Short description of the MIDI-DIN message of button b, "" if none. */
  void midiSummary(int b, char* out, size_t cap) const;

 private:
  void runAction(int b);
  void sendMidi(int b, bool pressed);
  void pedal(int p, int value);
  int resolveBlock(uint8_t target) const;

  AppConfig* cfg_ = nullptr;
  AmperoClient* ampero_ = nullptr;
  MidiDin* din_ = nullptr;
  ExpPins pins_{};
  bool midiToggle_[8] = {false};
  // EXP ports
  int32_t filt_[2] = {0, 0};      // ADC value << 4
  int expSent_[2] = {-1, -1};
  int rawAtSent_[2] = {0, 0};
  uint32_t lastSample_ = 0, lastSend_[2] = {0, 0};
  bool swIdle_[2] = {true, true}, swLevel_[2] = {true, true}, swPressed_[2] = {false, false};
  uint32_t swChange_[2] = {0, 0};
  int ampExpSelect_ = -1;         // last CC 13 sent (0 = EXP 1, 1 = EXP 2)
  uint32_t revision_ = 0;
  uint32_t expRevision_ = 0;
};
