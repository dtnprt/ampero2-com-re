#pragma once
/*
 * MIDI-DIN on UART0 (Serial1, 31250 Baud): sends whole messages, parses incoming bytes.
 * Wiring (3.3 V): see README, section "MIDI-DIN".
 */
#include <Arduino.h>

#include "MidiParser.h"

class MidiDin {
 public:
  using Handler = void (*)(const uint8_t* msg, size_t len);

  void begin(uint8_t txPin, uint8_t rxPin);
  /** Sends one complete message (channel, system, realtime or SysEx). */
  void send(const uint8_t* msg, size_t len);
  void send3(uint8_t status, uint8_t d1, uint8_t d2);
  void send2(uint8_t status, uint8_t d1);
  /** Reads what has arrived and calls handler for every complete message. */
  void poll(Handler handler);

  uint32_t rxCount() const { return rxCount_; }
  uint32_t txCount() const { return txCount_; }

 private:
  MidiParser<400> parser_;
  bool started_ = false;
  uint32_t rxCount_ = 0, txCount_ = 0;
};
