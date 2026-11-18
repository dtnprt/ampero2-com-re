#pragma once
/*
 * Ampero II (Stomp) SysEx client: asks the device for the current preset and scene,
 * keeps preset name, scene names and active scene up to date.
 *
 * Frame: F0 21 25 4D 50 00 00 | cs | kind | totLo totHi | offLo offHi | nibbles | F7
 * Stream: u32 cmd, u32 len, LZO1X(payload)   (see tools/FORMAT.md, section 6)
 */
#include <Arduino.h>

class AmperoClient {
 public:
  using SendFn = bool (*)(const uint8_t* frame, size_t len);

  void begin(SendFn send) { send_ = send; }
  void onConnected();
  void onDisconnected();
  void onSysEx(const uint8_t* msg, size_t len);
  /** Polling and timeouts; call often. */
  void loop();

  // ---- state for the display ----
  bool online() const { return online_; }
  bool hasPreset() const { return presetIndex_ >= 0; }
  int presetIndex() const { return presetIndex_; }
  const char* presetName() const { return presetName_; }
  const char* sceneName(int i) const { return sceneNames_[i]; }
  int activeScene() const { return activeScene_; }
  /** Increments whenever something visible changed. */
  uint32_t revision() const { return revision_; }

  /** Selects scene 0..2 on the device. */
  void selectScene(int scene);

 private:
  void request(uint32_t cmd, const uint8_t* payload, size_t n);
  void sendTransfer(uint32_t cmd, const uint8_t* payload, size_t n, uint8_t kind);
  void handleMessage(uint32_t cmd, const uint8_t* payload, size_t n);
  bool parsePreset(const uint8_t* tlv, size_t n);
  void setScene(int s);

  SendFn send_ = nullptr;
  bool online_ = false;
  uint32_t pending_ = 0;          // command we wait for, 0 = idle
  uint32_t pendingSince_ = 0;
  uint32_t lastPoll_ = 0;
  uint32_t connectedAt_ = 0;
  bool chainScene_ = false;       // after the preset answer, also ask for the scene

  int presetIndex_ = -1;
  int wantedPreset_ = -1;
  char presetName_[17] = "";
  char sceneNames_[3][9] = {"", "", ""};
  int activeScene_ = -1;
  uint32_t revision_ = 0;

  // reassembly of one transfer
  uint8_t stream_[4096];
  size_t streamTotal_ = 0, streamFill_ = 0;
  bool streamOk_ = false;
};
