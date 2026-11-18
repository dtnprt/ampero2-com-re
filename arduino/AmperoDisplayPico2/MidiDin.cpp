#include "MidiDin.h"

void MidiDin::begin(uint8_t txPin, uint8_t rxPin) {
  Serial1.setTX(txPin);
  Serial1.setRX(rxPin);
  Serial1.setFIFOSize(512);   // the display loop can be busy for ~100 ms while drawing
  Serial1.begin(31250);
  started_ = true;
}

void MidiDin::send(const uint8_t* msg, size_t len) {
  if (!started_ || !len) return;
  Serial1.write(msg, len);
  txCount_++;
}

void MidiDin::send3(uint8_t status, uint8_t d1, uint8_t d2) {
  const uint8_t m[3] = {status, (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F)};
  send(m, 3);
}

void MidiDin::send2(uint8_t status, uint8_t d1) {
  const uint8_t m[2] = {status, (uint8_t)(d1 & 0x7F)};
  send(m, 2);
}

void MidiDin::poll(Handler handler) {
  if (!started_) return;
  int budget = 512;
  while (budget-- > 0 && Serial1.available() > 0) {
    if (parser_.feed((uint8_t)Serial1.read())) {
      rxCount_++;
      if (handler) handler(parser_.msg(), parser_.len());
    }
  }
}
