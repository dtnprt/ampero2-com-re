#pragma once
/*
 * Minimal USB-MIDI host for the ESP32-S3 (ESP-IDF usb_host library, arduino-esp32 core 3.x).
 * Opens the first device with a MIDI-Streaming interface (class 1 / subclass 3, bulk IN+OUT),
 * reassembles incoming SysEx messages and sends SysEx as USB-MIDI event packets.
 * The ESP32-S3 OTG port is full-speed only; high-speed devices (like the Ampero II) must
 * fall back to full speed - endpoint sizes are therefore read from the descriptors.
 */
#include <Arduino.h>
#include <freertos/stream_buffer.h>

class UsbMidiHost {
 public:
  bool begin();
  bool connected() const { return connected_; }
  uint16_t vid() const { return vid_; }
  uint16_t pid() const { return pid_; }

  /** Sends one complete SysEx message (F0 ... F7). Blocks until the USB transfer is done. */
  bool sendSysEx(const uint8_t* msg, size_t len);

  /**
   * Polls received MIDI data. Returns the length of a complete SysEx (F0..F7) copied to buf,
   * 0 if none is ready. Call repeatedly from loop().
   */
  size_t readSysEx(uint8_t* buf, size_t cap);

  /** Incremented on every connect/disconnect. */
  uint32_t connectionEpoch() const { return epoch_; }

 private:
  static void libTask(void* arg);
  static void clientTask(void* arg);
  static void clientEvent(const void* msg, void* arg);
  static void inDone(void* transfer);
  static void outDone(void* transfer);
  void openDevice(uint8_t address);
  void closeDevice();

  void* client_ = nullptr;
  void* device_ = nullptr;
  void* inXfer_ = nullptr;
  void* outXfer_ = nullptr;
  int interface_ = -1;
  uint8_t epIn_ = 0, epOut_ = 0;
  uint16_t mpsIn_ = 64, mpsOut_ = 64;
  uint16_t vid_ = 0, pid_ = 0;
  volatile bool connected_ = false;
  volatile uint8_t pendingOpen_ = 0;
  volatile bool pendingClose_ = false;
  volatile uint32_t epoch_ = 0;
  StreamBufferHandle_t rx_ = nullptr;
  SemaphoreHandle_t outSem_ = nullptr;
  SemaphoreHandle_t outLock_ = nullptr;

  // SysEx assembly (loop context)
  uint8_t sx_[512];
  size_t sxLen_ = 0;
  bool sxActive_ = false;
};
