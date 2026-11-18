#include "AmperoClient.h"

#define LOGF(...) Serial0.printf(__VA_ARGS__)

namespace {

constexpr uint8_t kHeader[7] = {0xF0, 0x21, 0x25, 0x4D, 0x50, 0x00, 0x00};
constexpr uint8_t kRequest = 0x11, kSet = 0x12;

constexpr uint32_t CMD_CONNECT = 0x05000017;
constexpr uint32_t CMD_CURRENT_PRESET = 0x00090000;
constexpr uint32_t CMD_PRESET_DUMP = 0x01000000;
constexpr uint32_t CMD_CURRENT_SCENE = 0x03000001;

constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kTimeoutMs = 1500;

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

void AmperoClient::request(uint32_t cmd, const uint8_t* payload, size_t n) {
  pending_ = cmd;
  pendingSince_ = millis();
  sendTransfer(cmd, payload, n, kRequest);
}

void AmperoClient::selectScene(int scene) {
  if (!online_ || scene < 0 || scene > 2) return;
  uint8_t s = (uint8_t)scene;
  sendTransfer(CMD_CURRENT_SCENE, &s, 1, kSet);
  setScene(scene);
}

// ------------------------------------------------------------------ connection / polling

void AmperoClient::onConnected() {
  online_ = true;
  pending_ = 0;
  presetIndex_ = -1;
  wantedPreset_ = -1;
  activeScene_ = -1;
  streamFill_ = streamTotal_ = 0;
  connectedAt_ = millis();
  lastPoll_ = 0;
  sendTransfer(CMD_CONNECT, nullptr, 0, kRequest);
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
  if (pending_ && now - pendingSince_ > kTimeoutMs) {
    LOGF("Ampero: keine Antwort auf %08lx\r\n", (unsigned long)pending_);
    pending_ = 0;
  }
  if (pending_ || now - connectedAt_ < 200) return;

  if (wantedPreset_ >= 0) {                       // a preset change is known -> fetch its dump
    uint32_t idx = (uint32_t)wantedPreset_;
    wantedPreset_ = -1;
    request(CMD_PRESET_DUMP, (const uint8_t*)&idx, 4);
  } else if (chainScene_) {
    chainScene_ = false;
    request(CMD_CURRENT_SCENE, nullptr, 0);
  } else if (lastPoll_ == 0 || now - lastPoll_ >= kPollMs) {
    lastPoll_ = now;
    chainScene_ = true;
    request(CMD_CURRENT_PRESET, nullptr, 0);
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

  const uint32_t cmd = rd32(stream_);
  const uint32_t lzoLen = rd32(stream_ + 4);
  if (8 + lzoLen > streamTotal_) return;
  if (cmd != CMD_CURRENT_PRESET && cmd != CMD_PRESET_DUMP && cmd != CMD_CURRENT_SCENE) return;   // not needed here
  int plain = 0;
  if (lzoLen) {
    plain = lzoDecompress(stream_ + 8, lzoLen, g_plain, sizeof g_plain);
    if (plain < 0) {
      LOGF("Ampero: LZO-Fehler bei %08lx\r\n", (unsigned long)cmd);
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
        LOGF("Ampero: Preset %d \"%s\" [%s | %s | %s]\r\n", presetIndex_, presetName_, sceneNames_[0], sceneNames_[1], sceneNames_[2]);
        revision_++;
      }
      break;
    case CMD_CURRENT_SCENE:
      if (n >= 1) setScene(p[0]);
      break;
    default:
      break;
  }
}

void AmperoClient::setScene(int s) {
  if (s < 0 || s > 2 || s == activeScene_) return;
  activeScene_ = s;
  revision_++;
}

/** TLV preset: tag 1 = info (u32 version, char name[17], ...), tag 5 = scenes (ends with char name[3][8]). */
bool AmperoClient::parsePreset(const uint8_t* tlv, size_t n) {
  bool haveName = false, haveScenes = false;
  size_t o = 0;
  while (o + 8 <= n) {
    const uint32_t tag = rd32(tlv + o), len = rd32(tlv + o + 4);
    const uint8_t* d = tlv + o + 8;
    if (o + 8 + len > n) return false;
    if (tag == 1 && len >= 21) {
      copyName(presetName_, sizeof presetName_, d + 4, 17);
      haveName = true;
    } else if (tag == 5 && len >= 24) {
      for (int s = 0; s < 3; s++) copyName(sceneNames_[s], sizeof sceneNames_[s], d + len - 24 + 8 * s, 8);
      haveScenes = true;
    }
    o += 8 + len;
  }
  return haveName && haveScenes;
}
