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
  /** What footswitch i (0..2) does, e.g. "Tap Tempo" or the effects it switches, one per line ('\n'). */
  const char* footswitchText(int i) const { return fsText_[i]; }
  /** State of footswitch i: 1 = on, 0 = off, -1 = no state (e.g. Tap Tempo). Ctrl 1..3: switched effects on in
   *  the active scene; Scene n: scene n active. */
  int footswitchState(int i) const;
  /** Increments whenever something visible changed. */
  uint32_t revision() const { return revision_; }

  // ---- effect slots of the current preset ----
  bool slotUsed(int s) const { return s >= 0 && s < 12 && slotUsed_[s]; }
  /** Effect category (Categories.h), -1 = empty slot. */
  int slotCategory(int s) const { return slotUsed(s) && slotModule_[s] < 0x80 ? slotModule_[s] : -1; }
  /** Effect name, "" if unknown/empty. */
  const char* slotName(int s) const;
  /** Effect on in the active scene. */
  bool slotOn(int s) const;
  /** First used slot of the category, -1 if the preset has none. */
  int findSlot(int category) const;
  /** Switches the effect in slot s on/off in the active scene (like the editor: SysEx 0x01090004). */
  void toggleSlot(int s);

  // ---- device footswitches ----
  uint8_t footswitchFunction(int fs) const { return fs >= 0 && fs < 3 ? fsFunc_[fs] : 0xFF; }
  /** Category of the first effect a "Ctrl n" footswitch switches, -1 otherwise. */
  int footswitchCategory(int fs) const;
  /** Does what device footswitch fs (0..2) does: Ctrl -> effects, Scene -> scene, others via MIDI CC. */
  void performFootswitch(int fs);

  /** Selects scene 0..2 on the device (SysEx like the editor, plus CC 25 if a channel is set). */
  void selectScene(int scene);
  /** MIDI channel 1..16 for CC commands (scene CC 25, footswitch functions, expression), 0 = none. */
  void setMidiChannel(int channel) { ccChannel_ = channel; }
  /** CC to the Ampero on its MIDI channel. */
  void sendCc(uint8_t cc, uint8_t value);
  /** Any MIDI message to the Ampero (e.g. forwarded from MIDI-DIN). */
  bool sendRaw(const uint8_t* msg, size_t len) { return online_ && send_ && send_(msg, len); }

 private:
  int ctrlFirstSlot(int ctrl) const;
  void toggleCtrl(int ctrl);
  void sendSlotStates(int scene);
  void toggleCc(uint8_t cc, uint8_t on, uint8_t off);
  void request(uint32_t cmd, const uint8_t* payload, size_t n, uint32_t timeoutMs = 500);
  void sendTransfer(uint32_t cmd, const uint8_t* payload, size_t n, uint8_t kind);
  void processStream(const uint8_t* s, size_t total);
  void handleMessage(uint32_t cmd, const uint8_t* payload, size_t n);
  bool parsePreset(const uint8_t* tlv, size_t n);
  void buildFootswitchText(int fs, uint8_t func, const uint32_t* algs, const uint8_t* ctrlSlots);
  void setScene(int s);

  SendFn send_ = nullptr;
  int ccChannel_ = 1;
  bool online_ = false;
  uint32_t pending_ = 0;          // command we wait for, 0 = idle
  uint32_t pendingSince_ = 0;
  uint32_t lastPoll_ = 0;
  uint32_t lastScenePoll_ = 0;
  uint32_t pendingTimeout_ = 500;
  int initStep_ = 0;                // next query of the editor's start-up sequence
  uint32_t sceneHoldUntil_ = 0;     // ignore scene answers until then (after selectScene)
  uint32_t connectedAt_ = 0;
  bool chainScene_ = false;       // after the preset answer, also ask for the scene
  int handshakeLeft_ = 0;         // short 7F frames still to send after connect (like the editor)
  uint32_t lastHandshake_ = 0;

  int presetIndex_ = -1;
  int wantedPreset_ = -1;
  char presetName_[17] = "";
  char sceneNames_[3][9] = {"", "", ""};
  char fsText_[3][64] = {"", "", ""};
  uint8_t fsFunc_[3] = {0xFF, 0xFF, 0xFF};
  uint8_t ctrlSlots_[36] = {0};     // Ctrl 1..3 -> slots they switch
  bool slotUsed_[12] = {false};
  uint8_t slotModule_[12] = {0};    // category per slot (tag 4, bytes 12..23)
  uint32_t slotAlg_[12] = {0};
  bool ccToggled_[128] = {false};   // state of on/off CC functions (Tuner, Looper, ...)
  bool slotOn_[3][12] = {{false}};  // effect on/off per scene and slot
  bool gotSlotState_ = false;       // device sent CMD_SLOT_STATE since connect
  int activeScene_ = -1;
  uint32_t revision_ = 0;

  // reassembly of one transfer
  uint8_t stream_[4096];
  uint8_t single_[200];             // single-frame messages (decoded without touching stream_)
  int pendingDump_ = -1;            // preset index of the dump in flight
  size_t streamTotal_ = 0, streamFill_ = 0;
  bool streamOk_ = false;
};
