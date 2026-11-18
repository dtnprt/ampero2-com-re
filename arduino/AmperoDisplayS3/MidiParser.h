#pragma once
/*
 * Byte-stream MIDI parser: channel messages (with running status), system common, realtime
 * (also in the middle of other messages) and SysEx up to N bytes. feed() returns true when a
 * message is complete; it is then available through msg()/len() until the next feed().
 */
#include <stddef.h>
#include <stdint.h>

template <size_t N>
class MidiParser {
 public:
  bool feed(uint8_t b) {
    if (b >= 0xF8) {                       // realtime: single byte, does not disturb anything else
      rt_ = b;
      msg_ = &rt_;
      len_ = 1;
      return true;
    }
    if (b == 0xF0) {
      sysex_ = true;
      overflow_ = false;
      status_ = 0;
      n_ = 0;
      buf_[n_++] = b;
      return false;
    }
    if (b == 0xF7) {
      if (!sysex_) return false;
      sysex_ = false;
      if (overflow_ || n_ >= N) return false;   // too long for the buffer: dropped
      buf_[n_++] = b;
      return done(n_);
    }
    if (b & 0x80) {                        // status byte
      sysex_ = false;
      status_ = b;
      n_ = 0;
      buf_[n_++] = b;
      if (b >= 0xF0) need_ = (b == 0xF1 || b == 0xF3) ? 1 : (b == 0xF2) ? 2 : 0;   // system common
      else need_ = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
      if (need_ == 0) {
        status_ = 0;
        return done(1);
      }
      return false;
    }
    if (sysex_) {                          // data byte
      if (n_ < N) buf_[n_++] = b;
      else overflow_ = true;
      return false;
    }
    if (!status_) return false;            // data without status
    if (n_ == 0) buf_[n_++] = status_;     // running status
    buf_[n_++] = b;
    if (n_ < (size_t)need_ + 1) return false;
    const size_t len = n_;
    n_ = 0;
    if (status_ >= 0xF0) status_ = 0;      // no running status for system common
    return done(len);
  }

  const uint8_t* msg() const { return msg_; }
  size_t len() const { return len_; }
  void reset() {
    sysex_ = false;
    status_ = 0;
    n_ = 0;
  }

 private:
  bool done(size_t len) {
    msg_ = buf_;
    len_ = len;
    return true;
  }

  uint8_t buf_[N];
  uint8_t rt_ = 0;
  const uint8_t* msg_ = buf_;
  size_t len_ = 0, n_ = 0;
  uint8_t status_ = 0, need_ = 0;
  bool sysex_ = false, overflow_ = false;
};
