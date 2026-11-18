#include "AmperoClient.h"

#include "AlgNames.h"

#if defined(ARDUINO_ARCH_RP2040)
#define LOGF(...) Serial.printf(__VA_ARGS__)
#else
#define LOGF(...) Serial0.printf(__VA_ARGS__)
#endif

namespace {

constexpr uint8_t kHeader[7] = {0xF0, 0x21, 0x25, 0x4D, 0x50, 0x00, 0x00};
constexpr uint8_t kRequest = 0x11, kSet = 0x12;

constexpr uint32_t CMD_CONNECT = 0x05000017;
constexpr uint32_t CMD_CURRENT_PRESET = 0x00090000;
constexpr uint32_t CMD_PRESET_DUMP = 0x01000000;
constexpr uint32_t CMD_CURRENT_SCENE = 0x03000001;
constexpr uint32_t CMD_PRESET_CHANGED = 0x03000003;   // HT_COMD_CODE_ADDR_PRESET_CHANGE
constexpr uint32_t CMD_INPUT_LEVEL = 0x03000006;
constexpr uint32_t CMD_OUTPUT_LEVEL = 0x03000007;
constexpr uint32_t CMD_SLOT_TOTAL = 0x01040004;       // HT_COMD_CODE_ADDR_PRESET_SLOT_TOTAL
constexpr uint32_t CMD_SLOT_STATE = 0x01090004;       // ..._PRESET_SCENE_EFFECT_SLOT_ALGRO_STATE

// the editor sends this short frame 5x every 200 ms after CONNECT; the device answers requests only afterwards
constexpr uint8_t kHandshake[16] = {0xF0, 0x21, 0x25, 0x7F, 0x4D, 0x50, 0x01, 0x2C,
                                    0x11, 0x00, 0x00, 0x38, 0x00, 0x0E, 0x04, 0xF7};
constexpr int kHandshakeCount = 5;
constexpr uint32_t kHandshakeMs = 200;

constexpr uint32_t kPollMs = 1000;        // current preset
constexpr uint32_t kScenePollMs = 200;    // current scene
// request timeout: 500 ms by default (capture: short answers 10..35 ms, preset dump ~110 ms)
constexpr uint32_t kInitTimeoutMs = 1500;   // preset list: 13 frames ~50 ms apart

/** Rest of the editor's start-up sequence after preset + scene (capture, in this order). */
struct InitQuery {
  uint32_t cmd;
  uint8_t payloadLen;   // payload = zeros
};
const InitQuery kInitQueries[] = {
    {0x01080000, 0},   // preset list (names)
    {0x03000005, 0},   // firmware version
    {0x000a0000, 0},
    {0x03000003, 0},   // preset changed flag
    {0x04000003, 0},   // user IR names
    {0x0300000a, 0},   // algorithm times
    {0x07000003, 0},
    {0x08000003, 0},
    {0x05000013, 0},
    {0x00070000, 0},   // global settings (tempo ...)
    {0x010a0003, 4},   // slot time, u32 0
};

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// decompressed preset dump: u32 index + TLV (5068 bytes on firmware 2.5)
uint8_t g_plain[8192];

/** LZO1X decompression with bounds checks. Returns output length or -1. */
int lzoDecompress(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap) {
  size_t ip = 0, op = 0;
  unsigned t = 0;
  enum { LOOP, FIRST_LIT, MATCH, MATCH_DONE } state = LOOP;
  auto need = [&](size_t n) { return ip + n <= inLen; };
  auto lits = [&](size_t n) {
    if (!need(n) || op + n > outCap) return false;
    memcpy(out + op, in + ip, n);
    ip += n;
    op += n;
    return true;
  };
  auto copy = [&](long pos, size_t n) {
    if (pos < 0 || op + n > outCap) return false;
    for (size_t i = 0; i < n; i++) out[op + i] = out[pos + i];
    op += n;
    return true;
  };
  auto ext = [&](unsigned base) -> long {
    unsigned v = 0;
    while (need(1) && in[ip] == 0) { v += 255; ip++; }
    if (!need(1)) return -1;
    return v + base + in[ip++];
  };

  if (!need(1)) return -1;
  if (in[0] > 17) {
    t = in[0] - 17;
    ip = 1;
    if (!lits(t)) return -1;
    if (t < 4) {
      if (!need(1)) return -1;
      t = in[ip++];
      state = MATCH;
    } else {
      state = FIRST_LIT;
    }
  }
  for (;;) {
    switch (state) {
      case LOOP: {
        if (!need(1)) return -1;
        t = in[ip++];
        if (t >= 16) { state = MATCH; break; }
        if (t == 0) { long e = ext(15); if (e < 0) return -1; t = e; }
        if (!lits(t + 3)) return -1;
        state = FIRST_LIT;
        break;
      }
      case FIRST_LIT: {
        if (!need(1)) return -1;
        t = in[ip++];
        if (t >= 16) { state = MATCH; break; }
        if (!need(1)) return -1;
        if (!copy((long)op - 0x801 - (t >> 2) - (in[ip++] << 2), 3)) return -1;
        state = MATCH_DONE;
        break;
      }
      case MATCH: {
        if (t >= 64) {
          if (!need(1)) return -1;
          if (!copy((long)op - 1 - ((t >> 2) & 7) - (in[ip++] << 3), (t >> 5) + 1)) return -1;
        } else if (t >= 32) {
          t &= 31;
          if (t == 0) { long e = ext(31); if (e < 0) return -1; t = e; }
          if (!need(2)) return -1;
          long pos = (long)op - 1 - ((in[ip] | (in[ip + 1] << 8)) >> 2);
          ip += 2;
          if (!copy(pos, t + 2)) return -1;
        } else if (t >= 16) {
          long pos = (long)op - ((t & 8) << 11);
          t &= 7;
          if (t == 0) { long e = ext(7); if (e < 0) return -1; t = e; }
          if (!need(2)) return -1;
          pos -= (in[ip] | (in[ip + 1] << 8)) >> 2;
          ip += 2;
          if (pos == (long)op) return (int)op;   // end of stream
          if (!copy(pos - 0x4000, t + 2)) return -1;
        } else {
          if (!need(1)) return -1;
          if (!copy((long)op - 1 - (t >> 2) - (in[ip++] << 2), 2)) return -1;
        }
        state = MATCH_DONE;
        break;
      }
      case MATCH_DONE: {
        t = in[ip - 2] & 3;
        if (t == 0) { state = LOOP; break; }
        if (!lits(t) || !need(1)) return -1;
        t = in[ip++];
        state = MATCH;
        break;
      }
    }
  }
}

void copyName(char* dst, size_t cap, const uint8_t* src, size_t n) {
  size_t i = 0;
  for (; i < n && i + 1 < cap && src[i]; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
  dst[i] = 0;
}

}  // namespace

// ------------------------------------------------------------------ sending

void AmperoClient::sendTransfer(uint32_t cmd, const uint8_t* payload, size_t n, uint8_t kind) {
  // payloads sent by this client are tiny: LZO stream = literal run (17+n) + data + end marker,
  // which is exactly what the editor's lzo1x_1_compress produces for n <= 20.
  uint8_t stream[8 + 1 + 32 + 3];
  size_t lzoLen = n ? 1 + n + 3 : 0;
  memcpy(stream, &cmd, 4);
  uint32_t l = lzoLen;
  memcpy(stream + 4, &l, 4);
  if (n) {
    stream[8] = (uint8_t)(17 + n);
    memcpy(stream + 9, payload, n);
    stream[9 + n] = 0x11;
    stream[10 + n] = 0x00;
    stream[11 + n] = 0x00;
  }
  const size_t total = 8 + lzoLen;

  uint8_t frame[14 + 2 * sizeof stream];
  memcpy(frame, kHeader, 7);
  unsigned sum = 0;
  for (size_t i = 0; i < total; i++) {
    frame[13 + 2 * i] = stream[i] >> 4;
    frame[14 + 2 * i] = stream[i] & 0x0F;
    sum += (stream[i] >> 4) + (stream[i] & 0x0F);
  }
  frame[7] = sum & 0x7F;
  frame[8] = kind;
  frame[9] = total & 0x7F;
  frame[10] = total >> 7;
  frame[11] = 0;
  frame[12] = 0;
  frame[13 + 2 * total] = 0xF7;
  if (send_) send_(frame, 14 + 2 * total);
}

void AmperoClient::request(uint32_t cmd, const uint8_t* payload, size_t n, uint32_t timeoutMs) {
  pending_ = cmd;
  pendingSince_ = millis();
  pendingTimeout_ = timeoutMs;
  sendTransfer(cmd, payload, n, kRequest);
}

void AmperoClient::selectScene(int scene) {
  if (!online_ || scene < 0 || scene > 2) return;
  uint8_t s = (uint8_t)scene;
  sendTransfer(CMD_CURRENT_SCENE, &s, 1, kSet);   // what the editor sends
  sceneHoldUntil_ = millis() + 400;
  if (ccChannel_ >= 1 && ccChannel_ <= 16 && send_) {
    // documented MIDI implementation: CC 25, value 1..3 = scene 1..3
    const uint8_t cc[3] = {(uint8_t)(0xB0 | (ccChannel_ - 1)), 25, (uint8_t)(scene + 1)};
    send_(cc, sizeof cc);
  }
  setScene(scene);
}

// ------------------------------------------------------------------ connection / polling

void AmperoClient::onConnected() {
  online_ = true;
  pending_ = 0;
  presetIndex_ = -1;
  wantedPreset_ = -1;
  activeScene_ = -1;
  gotSlotState_ = false;
  initStep_ = 0;
  streamFill_ = streamTotal_ = 0;
  connectedAt_ = millis();
  lastPoll_ = 0;
  sendTransfer(CMD_CONNECT, nullptr, 0, kRequest);
  handshakeLeft_ = kHandshakeCount;
  lastHandshake_ = connectedAt_ - kHandshakeMs + 60;   // first one 60 ms after CONNECT
  revision_++;
}

void AmperoClient::onDisconnected() {
  online_ = false;
  pending_ = 0;
  presetIndex_ = -1;
  revision_++;
}

void AmperoClient::loop() {
  if (!online_) return;
  const uint32_t now = millis();
  if (handshakeLeft_ > 0) {
    if (now - lastHandshake_ >= kHandshakeMs && send_ && send_(kHandshake, sizeof kHandshake)) {
      lastHandshake_ = now;
      if (--handshakeLeft_ == 0) connectedAt_ = now;   // polling starts 200 ms later
    }
    return;
  }
  if (pending_ && now - pendingSince_ > pendingTimeout_) {
    LOGF("[%lu] Ampero: no answer to %08lx\r\n", (unsigned long)now, (unsigned long)pending_);
    if (pending_ == CMD_PRESET_DUMP && pendingDump_ >= 0 && wantedPreset_ < 0) wantedPreset_ = pendingDump_;   // retry
    pending_ = 0;
  }
  if (pending_ || now - connectedAt_ < 200) return;

  if (wantedPreset_ >= 0) {                       // a preset change is known -> fetch its dump
    uint32_t idx = (uint32_t)wantedPreset_;
    pendingDump_ = wantedPreset_;
    wantedPreset_ = -1;
    request(CMD_PRESET_DUMP, (const uint8_t*)&idx, 4);
  } else if (chainScene_) {
    chainScene_ = false;
    lastScenePoll_ = now;
    request(CMD_CURRENT_SCENE, nullptr, 0);
  } else if (hasPreset() && initStep_ < (int)(sizeof kInitQueries / sizeof kInitQueries[0])) {
    // once after connecting: the other queries of the editor's start-up sequence (in its order).
    // The device reports changes made on it by itself; this makes the Pico look like the editor.
    const InitQuery& q = kInitQueries[initStep_++];
    const uint8_t zero[4] = {0, 0, 0, 0};
    request(q.cmd, zero, q.payloadLen, kInitTimeoutMs);
    if (initStep_ == (int)(sizeof kInitQueries / sizeof kInitQueries[0]))
      LOGF("[%lu] Ampero: start-up queries sent\r\n", (unsigned long)now);
  } else if (lastPoll_ == 0 || now - lastPoll_ >= kPollMs) {
    lastPoll_ = now;
    chainScene_ = true;
    request(CMD_CURRENT_PRESET, nullptr, 0);
  } else if (hasPreset() && now - lastScenePoll_ >= kScenePollMs) {
    // scene changes on the device: ask often, in case the device does not report them by itself
    lastScenePoll_ = now;
    request(CMD_CURRENT_SCENE, nullptr, 0);
  }
}

// ------------------------------------------------------------------ receiving

void AmperoClient::onSysEx(const uint8_t* m, size_t len) {
  if (len < 14 || memcmp(m, kHeader, 7) != 0 || m[len - 1] != 0xF7) return;
  const size_t nib = len - 14;
  if (nib & 1) return;
  const size_t total = m[9] | (m[10] << 7);
  const size_t off = m[11] | (m[12] << 7);
  const size_t n = nib / 2;

  // A complete message in one frame (scene change, slot states, ...) can arrive between the frames of a
  // multi-frame answer (the device sends preset dumps in frames ~50 ms apart): decode it on its own
  // without disturbing the reassembly in progress.
  if (off == 0 && n == total && total >= 8 && total <= sizeof single_) {
    for (size_t i = 0; i < n; i++) single_[i] = (m[13 + 2 * i] << 4) | (m[14 + 2 * i] & 0x0F);
    processStream(single_, total);
    return;
  }

  if (off == 0) {
    streamTotal_ = total;
    streamFill_ = 0;
    streamOk_ = total <= sizeof stream_;
  }
  if (!streamOk_ || off != streamFill_ || streamFill_ + n > streamTotal_) {
    streamOk_ = false;
    return;
  }
  for (size_t i = 0; i < n; i++) stream_[streamFill_ + i] = (m[13 + 2 * i] << 4) | (m[14 + 2 * i] & 0x0F);
  streamFill_ += n;
  if (streamFill_ < streamTotal_ || streamTotal_ < 8) return;
  streamOk_ = false;
  processStream(stream_, streamTotal_);
}

/** One reassembled transfer: u32 cmd, u32 lzo length, LZO1X payload. */
void AmperoClient::processStream(const uint8_t* s, size_t total) {
  const uint32_t cmd = rd32(s);
  const uint32_t lzoLen = rd32(s + 4);
  if (8 + lzoLen > total) return;
  if (cmd == CMD_INPUT_LEVEL || cmd == CMD_OUTPUT_LEVEL) return;   // meters: not needed, frequent
  if (cmd == 0x01080000 || cmd == 0x04000003 || cmd == 0x0300000a || cmd == 0x07000003 || cmd == 0x08000003) {
    if (cmd == pending_) pending_ = 0;   // start-up answers we do not need (some exceed g_plain)
    return;
  }
  int plain = 0;
  if (lzoLen) {
    plain = lzoDecompress(s + 8, lzoLen, g_plain, sizeof g_plain);
    if (plain < 0) {
      LOGF("Ampero: LZO error in %08lx\r\n", (unsigned long)cmd);
      return;
    }
  }
  handleMessage(cmd, g_plain, (size_t)plain);
}

void AmperoClient::handleMessage(uint32_t cmd, const uint8_t* p, size_t n) {
  const bool answer = cmd == pending_;
  if (answer) pending_ = 0;

  switch (cmd) {
    case CMD_CURRENT_PRESET:
      if (n >= 4) {
        int idx = (int)rd32(p);
        if (idx != presetIndex_) wantedPreset_ = idx;   // fetched in loop()
      }
      break;
    case CMD_PRESET_DUMP:
      if (n > 4 && parsePreset(p + 4, n - 4)) {
        presetIndex_ = (int)rd32(p);
        chainScene_ = true;
        LOGF("[%lu] Ampero: preset %d \"%s\" [%s | %s | %s] (%lu ms)\r\n", (unsigned long)millis(), presetIndex_,
             presetName_, sceneNames_[0], sceneNames_[1], sceneNames_[2],
             (unsigned long)(millis() - pendingSince_));
        revision_++;
      }
      break;
    case CMD_CURRENT_SCENE:
      // shortly after our own switch, an answer to an older query could still report the old scene
      if (n >= 1 && (int32_t)(millis() - sceneHoldUntil_) >= 0) {
        if (p[0] != activeScene_)
          LOGF("[%lu] Ampero: scene %u (%s)\r\n", (unsigned long)millis(), p[0] + 1, answer ? "query" : "reported");
        setScene(p[0]);
      }
      break;
    case CMD_SLOT_STATE:   // u8 scene, s8 on[12] - sent by the device when effects are switched
      if (n >= 13 && p[0] < 3) {
        for (int i = 0; i < 12; i++) slotOn_[p[0]][i] = p[1 + i] > 0 && p[1 + i] < 0x80;
        gotSlotState_ = true;
        LOGF("[%lu] Ampero: slot states scene %u\r\n", (unsigned long)millis(), p[0] + 1);
        revision_++;
      }
      break;
    case CMD_SLOT_TOTAL:   // u32 slot, u32 category, u32 on, u32 model
      if (n >= 16 && rd32(p) < 12) {
        slotOn_[activeScene_ >= 0 ? activeScene_ : 0][rd32(p)] = rd32(p + 8) != 0;
        LOGF("[%lu] Ampero: slot %lu %s\r\n", (unsigned long)millis(), (unsigned long)rd32(p) + 1,
             rd32(p + 8) ? "on" : "off");
        revision_++;
      }
      break;
    case CMD_PRESET_CHANGED:   // something in the current preset changed: reload it, unless the
      // device reports slot states directly (the stored preset might lag behind those)
      LOGF("[%lu] Ampero: preset changed%s\r\n", (unsigned long)millis(), gotSlotState_ ? "" : " -> reload");
      if (presetIndex_ >= 0 && !gotSlotState_) wantedPreset_ = presetIndex_;
      break;
    default:
      if (!answer) {   // unknown notification: log it (helps to find out what the device sends)
        LOGF("[%lu] Ampero: message %08lx, %u bytes:", (unsigned long)millis(), (unsigned long)cmd, (unsigned)n);
        for (size_t i = 0; i < n && i < 20; i++) LOGF(" %02X", p[i]);
        LOGF("\r\n");
      }
      break;
  }
}

namespace {

/** Ctrl 1..3 of the Ampero II Stomp for a footswitch function code, -1 otherwise. */
int ctrlOf(uint8_t f) { return f == 12 ? 0 : (f == 32 || f == 33) ? f - 31 : -1; }

}  // namespace

int AmperoClient::ctrlFirstSlot(int ctrl) const {
  for (int s = 0; s < 12; s++)
    if (ctrlSlots_[ctrl * 12 + s] && slotUsed_[s]) return s;
  return -1;
}

int AmperoClient::footswitchState(int fs) const {
  if (fs < 0 || fs > 2) return -1;
  const uint8_t f = fsFunc_[fs];
  if (f >= 27 && f <= 29) return activeScene_ == f - 27 ? 1 : 0;   // Scene 1..3
  const int ctrl = ctrlOf(f);
  if (ctrl >= 0) {   // state of the first switched effect (they are switched together)
    const int s = ctrlFirstSlot(ctrl);
    return s >= 0 ? (slotOn(s) ? 1 : 0) : -1;
  }
  switch (f) {   // on/off functions sent as CC: our own idea of their state
    case 18: return ccToggled_[60] ? 1 : 0;   // Tuner
    case 17: return ccToggled_[62] ? 1 : 0;   // Looper
    case 48: return ccToggled_[36] ? 1 : 0;   // Drum
    default: return -1;
  }
}

int AmperoClient::footswitchCategory(int fs) const {
  if (fs < 0 || fs > 2) return -1;
  const int ctrl = ctrlOf(fsFunc_[fs]);
  return ctrl >= 0 ? slotCategory(ctrlFirstSlot(ctrl)) : -1;
}

const char* AmperoClient::slotName(int s) const {
  if (!slotUsed(s)) return "";
  const char* n = algName(slotAlg_[s]);
  return n ? n : "?";
}

bool AmperoClient::slotOn(int s) const {
  return slotUsed(s) && slotOn_[activeScene_ >= 0 ? activeScene_ : 0][s];
}

int AmperoClient::findSlot(int category) const {
  for (int s = 0; s < 12; s++)
    if (slotCategory(s) == category) return s;
  return -1;
}

void AmperoClient::sendSlotStates(int scene) {
  uint8_t p[13];
  p[0] = (uint8_t)scene;
  for (int i = 0; i < 12; i++) p[1 + i] = slotOn_[scene][i] ? 1 : 0;
  sendTransfer(CMD_SLOT_STATE, p, sizeof p, kSet);
  revision_++;
}

void AmperoClient::toggleSlot(int s) {
  if (!online_ || !slotUsed(s)) return;
  const int scene = activeScene_ >= 0 ? activeScene_ : 0;
  slotOn_[scene][s] = !slotOn_[scene][s];
  sendSlotStates(scene);
}

void AmperoClient::toggleCtrl(int ctrl) {
  const int first = ctrlFirstSlot(ctrl);
  if (!online_ || first < 0) return;
  const int scene = activeScene_ >= 0 ? activeScene_ : 0;
  const bool on = !slotOn_[scene][first];   // all switched effects follow the first one
  for (int s = 0; s < 12; s++)
    if (ctrlSlots_[ctrl * 12 + s] && slotUsed_[s]) slotOn_[scene][s] = on;
  sendSlotStates(scene);
}

void AmperoClient::sendCc(uint8_t cc, uint8_t value) {
  if (!online_ || !send_ || ccChannel_ < 1 || ccChannel_ > 16) return;
  const uint8_t m[3] = {(uint8_t)(0xB0 | (ccChannel_ - 1)), (uint8_t)(cc & 0x7F), (uint8_t)(value & 0x7F)};
  send_(m, sizeof m);
}

void AmperoClient::toggleCc(uint8_t cc, uint8_t on, uint8_t off) {
  ccToggled_[cc] = !ccToggled_[cc];
  sendCc(cc, ccToggled_[cc] ? on : off);
  revision_++;
}

void AmperoClient::performFootswitch(int fs) {
  if (fs < 0 || fs > 2 || !online_) return;
  const uint8_t f = fsFunc_[fs];
  const int ctrl = ctrlOf(f);
  if (ctrl >= 0) {
    toggleCtrl(ctrl);
    return;
  }
  if (f >= 27 && f <= 29) {
    selectScene(f - 27);
    return;
  }
  if (f < 12) {   // A1..B6 On/Off: CC 48..59 of the MIDI implementation
    toggleCc((uint8_t)(48 + f), 127, 0);
    return;
  }
  switch (f) {   // MIDI implementation of the Ampero II Stomp (CC numbers)
    case 13: sendCc(76, 127); break;          // Tap Tempo
    case 15: sendCc(23, 127); break;          // Bank +
    case 16: sendCc(22, 127); break;          // Bank -
    case 38: sendCc(27, 127); break;          // Patch +
    case 39: sendCc(26, 127); break;          // Patch -
    case 17: toggleCc(62, 127, 0); break;     // Looper menu
    case 18: toggleCc(60, 127, 0); break;     // Tuner
    case 19: toggleCc(78, 1, 2); break;       // Bypass: DSP bypass / engage
    case 41: toggleCc(13, 127, 0); break;     // EXP 1/2
    case 42: sendCc(63, 127); break;          // Looper Rec/Overdub
    case 43: sendCc(64, 0); break;            // Looper Stop
    case 44: toggleCc(65, 0, 127); break;     // Looper 1/2 speed
    case 45: toggleCc(66, 0, 127); break;     // Looper reverse
    case 47: sendCc(67, 127); break;          // Looper Undo/Redo
    case 48: toggleCc(36, 127, 0); break;     // Drum machine on/off
    case 49: ccToggled_[62] = false; sendCc(62, 0); break;   // Looper exit
    default: LOGF("Ampero: footswitch function %u not supported\r\n", f); break;
  }
}

void AmperoClient::setScene(int s) {
  if (s < 0 || s > 2 || s == activeScene_) return;
  activeScene_ = s;
  revision_++;
}

/**
 * TLV preset: tag 1 = info (u32 version, char name[17], ...), tag 3 = footswitch functions (u8 fs[3], ...),
 * tag 4 = slots (u8 order[12], u8 module[12], u32 alg[12]), tag 5 = scenes (ends with char name[3][8]),
 * tag 8 = ctrl (u8 led[3], u8 targetSlot[3][12]: which slots Ctrl 1..3 switch).
 */
bool AmperoClient::parsePreset(const uint8_t* tlv, size_t n) {
  bool haveName = false, haveScenes = false;
  uint8_t fsFunc[3] = {0xFF, 0xFF, 0xFF};
  uint32_t algs[12];
  uint8_t ctrl[36] = {0};
  for (int i = 0; i < 12; i++) algs[i] = 0xFFFFFFFF;
  size_t o = 0;
  while (o + 8 <= n) {
    const uint32_t tag = rd32(tlv + o), len = rd32(tlv + o + 4);
    const uint8_t* d = tlv + o + 8;
    if (o + 8 + len > n) return false;
    if (tag == 1 && len >= 21) {
      copyName(presetName_, sizeof presetName_, d + 4, 17);
      haveName = true;
    } else if (tag == 3 && len >= 3) {
      memcpy(fsFunc, d, 3);
    } else if (tag == 4 && len >= 72) {
      for (int i = 0; i < 12; i++) algs[i] = rd32(d + 24 + 4 * i);
      memcpy(slotModule_, d + 12, 12);
    } else if (tag == 5 && len >= 24) {
      for (int s = 0; s < 3; s++) copyName(sceneNames_[s], sizeof sceneNames_[s], d + len - 24 + 8 * s, 8);
      if (len >= 3636)   // u8 on[3][12] at 3600
        for (int s = 0; s < 3; s++)
          for (int i = 0; i < 12; i++) slotOn_[s][i] = d[3600 + 12 * s + i] > 0 && d[3600 + 12 * s + i] < 0x80;
      haveScenes = true;
    } else if (tag == 8 && len >= 39) {
      memcpy(ctrl, d + 3, 36);
    }
    o += 8 + len;
  }
  memcpy(fsFunc_, fsFunc, 3);
  memcpy(ctrlSlots_, ctrl, 36);
  for (int i = 0; i < 12; i++) {
    slotUsed_[i] = algs[i] != 0xFFFFFFFF;
    slotAlg_[i] = algs[i];
  }
  for (int fs = 0; fs < 3; fs++) buildFootswitchText(fs, fsFunc[fs], algs, ctrl);
  return haveName && haveScenes;
}

namespace {

/** Footswitch function codes of the editor (kFSMapFunc in the editor's Dart source). */
const char* footswitchFunctionName(uint8_t f) {
  static const char* const kSlots[12] = {"A1", "A2", "A3", "A4", "A5", "A6", "B1", "B2", "B3", "B4", "B5", "B6"};
  if (f < 12) return kSlots[f];   // caller appends " On/Off"
  switch (f) {
    case 13: return "Tap Tempo";
    case 15: return "Bank +";
    case 16: return "Bank -";
    case 17: return "Looper";
    case 18: return "Tuner";
    case 19: return "Bypass";
    case 20: return "Unit Mode";
    case 21: return "Ctrl Temp";
    case 22: return "Patch 1";
    case 23: return "Patch 2";
    case 24: return "Patch 3";
    case 25: return "Patch 4";
    case 26: return "Patch 5";
    case 27: return "Scene 1";
    case 28: return "Scene 2";
    case 29: return "Scene 3";
    case 30: return "Scene 4";
    case 31: return "Scene 5";
    case 38: return "Patch +";
    case 39: return "Patch -";
    case 40: return "Preset UI";
    case 41: return "EXP 1/2";
    case 42: return "Looper Rec/Play";
    case 43: return "Looper Stop";
    case 44: return "Looper 1/2";
    case 45: return "Looper Rev";
    case 46: return "Looper FX";
    case 47: return "Looper Undo";
    case 48: return "Drum On/Off";
    case 49: return "Looper Exit";
    case 50: return "Drum";
    case 0xFF: return "Off";
    default: return nullptr;
  }
}

/** Ctrl index for the "switch effect slots" functions: Ctrl 1 = 12, Ctrl 2..7 = 32..37; -1 otherwise. */
int ctrlIndex(uint8_t f) {
  if (f == 12) return 0;
  if (f >= 32 && f <= 37) return f - 31;
  return -1;
}

}  // namespace

void AmperoClient::buildFootswitchText(int fs, uint8_t func, const uint32_t* algs, const uint8_t* ctrlSlots) {
  char* out = fsText_[fs];
  const size_t cap = sizeof fsText_[fs];
  out[0] = 0;
  const int ctrl = ctrlIndex(func);
  if (ctrl >= 0 && ctrl < 3) {   // the Ampero II Stomp has Ctrl 1..3: list the effects they switch
    size_t len = 0;
    for (int s = 0; s < 12; s++) {
      if (!ctrlSlots[ctrl * 12 + s] || algs[s] == 0xFFFFFFFF) continue;
      const char* name = algName(algs[s]);
      len += snprintf(out + len, len < cap ? cap - len : 0, "%s%s", len ? "\n" : "", name ? name : "?");
      if (len >= cap) { out[cap - 1] = 0; break; }
    }
    if (!out[0]) snprintf(out, cap, "Ctrl %d", ctrl + 1);
    return;
  }
  if (ctrl >= 0) {
    snprintf(out, cap, "Ctrl %d", ctrl + 1);
  } else if (func < 12) {   // slot position A1..B6 (not mapped to an effect: position vs. slot index unverified)
    snprintf(out, cap, "%s On/Off", footswitchFunctionName(func));
  } else {
    const char* name = footswitchFunctionName(func);
    if (name) snprintf(out, cap, "%s", name);
    else snprintf(out, cap, "Function %u", func);
  }
}
