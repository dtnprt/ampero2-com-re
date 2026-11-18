#include "Controller.h"

#include <hardware/adc.h>
#include <hardware/gpio.h>
#include <stdio.h>
#include <string.h>

#include "Categories.h"

namespace {

constexpr uint16_t kSceneColor = rgb565c(245, 165, 36);
constexpr uint16_t kMidiColor = rgb565c(150, 156, 168);
constexpr uint32_t kSampleMs = 4;
constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kPedalMinGapMs = 12;

}  // namespace

void Controller::begin(AppConfig* cfg, AmperoClient* ampero, MidiDin* din, const ExpPins& pins) {
  cfg_ = cfg;
  ampero_ = ampero;
  din_ = din;
  pins_ = pins;
  adc_init();
  applyExpConfig();
}

void Controller::applyExpConfig() {
  for (int p = 0; p < 2; p++) {
    const uint8_t pin = pins_.pin[p];
    if (cfg_->exp[p].mode == EXP_SWITCH) {
      gpio_init(pin);
      gpio_set_dir(pin, GPIO_IN);
      gpio_pull_up(pin);
      delayMicroseconds(50);
      swLevel_[p] = swIdle_[p] = gpio_get(pin);   // idle level: works with normally open and normally closed
      swPressed_[p] = false;
    } else {
      adc_gpio_init(pin);                           // analog: no digital input, no pulls
      adc_select_input(pin - 26);
      filt_[p] = (int32_t)adc_read() << 4;
      expSent_[p] = -1;
    }
  }
  ampExpSelect_ = -1;
  revision_++;
}

int Controller::expRaw(int p) const {
  if (p < 0 || p > 1 || cfg_->exp[p].mode != EXP_PEDAL) return -1;
  return filt_[p] >> 4;
}

// ------------------------------------------------------------------ presses

void Controller::press(int b) {
  if (b < 0 || b > 7) return;
  runAction(b);
  sendMidi(b, true);
  revision_++;
}

void Controller::release(int b) {
  if (b < 0 || b > 7) return;
  sendMidi(b, false);
}

int Controller::resolveBlock(uint8_t target) const {
  if (target < 12) return ampero_->slotUsed(target) ? target : -1;
  if (target >= BLOCK_CATEGORY) return ampero_->findSlot(target - BLOCK_CATEGORY);
  return -1;
}

void Controller::runAction(int b) {
  const ButtonConfig& c = cfg_->buttons[b];
  if (!ampero_->online() || !ampero_->hasPreset()) return;
  switch (c.mode) {
    case BTN_SCENE:
      ampero_->selectScene(c.a);
      break;
    case BTN_SCENE_TOGGLE:
      ampero_->selectScene(ampero_->activeScene() == c.a ? c.b : c.a);
      break;
    case BTN_BLOCK: {
      const int s = resolveBlock(c.a);
      if (s >= 0) ampero_->toggleSlot(s);
      break;
    }
    case BTN_DEVICE_FS:
      for (int fs = 0; fs < 3; fs++)
        if (c.fsMask & (1 << fs)) ampero_->performFootswitch(fs);
      break;
    default:
      break;
  }
}

void Controller::sendMidi(int b, bool pressed) {
  const MidiAction& m = cfg_->buttons[b].midi;
  const uint8_t ch = m.channel & 0x0F;
  switch (m.type) {
    case MIDI_CC:
      if (!pressed) return;
      if (m.valueOn != m.valueOff) midiToggle_[b] = !midiToggle_[b];
      else midiToggle_[b] = true;
      din_->send3(0xB0 | ch, m.number, midiToggle_[b] ? m.valueOn : m.valueOff);
      break;
    case MIDI_PC:
      if (pressed) din_->send2(0xC0 | ch, m.number);
      break;
    case MIDI_NOTE:
      if (pressed) din_->send3(0x90 | ch, m.number, m.valueOn ? m.valueOn : 1);
      else din_->send3(0x80 | ch, m.number, 0);
      break;
    default:
      break;
  }
}

void Controller::midiSummary(int b, char* out, size_t cap) const {
  const MidiAction& m = cfg_->buttons[b].midi;
  switch (m.type) {
    case MIDI_CC: snprintf(out, cap, "CC %u Ch%u", m.number, m.channel + 1); break;
    case MIDI_PC: snprintf(out, cap, "PC %u Ch%u", m.number, m.channel + 1); break;
    case MIDI_NOTE: snprintf(out, cap, "Note %u Ch%u", m.number, m.channel + 1); break;
    default: snprintf(out, cap, "off"); break;
  }
}

// ------------------------------------------------------------------ tiles

void Controller::tileView(int i, TileView& v) const {
  v = TileView();
  const ButtonConfig& c = cfg_->buttons[i];
  switch (c.mode) {
    case BTN_SCENE: {
      snprintf(v.tag, sizeof v.tag, "SCENE %u", c.a + 1);
      const char* n = ampero_->sceneName(c.a);
      snprintf(v.text, sizeof v.text, "%s", n[0] ? n : "-");
      v.color = kSceneColor;
      v.state = ampero_->activeScene() == c.a ? 1 : 0;
      break;
    }
    case BTN_SCENE_TOGGLE: {
      const int active = ampero_->activeScene();
      snprintf(v.tag, sizeof v.tag, "SCENE %u/%u", c.a + 1, c.b + 1);
      // the scene of the pair that is active first
      const int first = active == c.b ? c.b : c.a, second = first == c.a ? c.b : c.a;
      snprintf(v.text, sizeof v.text, "%s\n%s", ampero_->sceneName(first), ampero_->sceneName(second));
      v.color = kSceneColor;
      v.state = (active == c.a || active == c.b) ? 1 : 0;
      break;
    }
    case BTN_BLOCK: {
      const int s = resolveBlock(c.a);
      const int cat = c.a >= BLOCK_CATEGORY ? c.a - BLOCK_CATEGORY : ampero_->slotCategory(s);
      if (cat >= 0) snprintf(v.tag, sizeof v.tag, "%s", categoryName(cat));
      else snprintf(v.tag, sizeof v.tag, "SLOT %u", c.a + 1);
      v.color = categoryColor(cat);
      if (s >= 0) {
        snprintf(v.text, sizeof v.text, "%s", ampero_->slotName(s));
        v.state = ampero_->slotOn(s) ? 1 : 0;
      } else {
        snprintf(v.text, sizeof v.text, "not in preset");
        v.state = -1;
      }
      break;
    }
    case BTN_DEVICE_FS: {
      int first = -1;
      size_t tl = 0, xl = 0;
      for (int fs = 0; fs < 3; fs++) {
        if (!(c.fsMask & (1 << fs))) continue;
        tl += snprintf(v.tag + tl, tl < sizeof v.tag ? sizeof v.tag - tl : 0, "%sFS%d", first < 0 ? "" : "+", fs + 1);
        const char* t = ampero_->footswitchText(fs);
        if (first < 0) {
          xl += snprintf(v.text + xl, sizeof v.text - xl, "%s", t);
        } else if (xl < sizeof v.text) {   // further footswitches: their first line
          const char* nl = strchr(t, '\n');
          xl += snprintf(v.text + xl, sizeof v.text - xl, "\n%.*s", nl ? (int)(nl - t) : (int)strlen(t), t);
        }
        if (first < 0) first = fs;
      }
      if (first < 0) {
        snprintf(v.text, sizeof v.text, "-");
        break;
      }
      const int cat = ampero_->footswitchCategory(first);
      if (cat >= 0 && tl < sizeof v.tag) snprintf(v.tag + tl, sizeof v.tag - tl, " %s", categoryName(cat));
      v.color = cat >= 0 ? categoryColor(cat) : 0;
      v.state = (int8_t)ampero_->footswitchState(first);
      if (v.state >= 0 && !v.color) v.color = rgb565c(120, 170, 255);
      break;
    }
    default:
      break;
  }
  if (c.mode == BTN_OFF) {
    if (c.midi.type != MIDI_OFF) {
      snprintf(v.tag, sizeof v.tag, "MIDI");
      midiSummary(i, v.text, sizeof v.text);
      v.color = kMidiColor;
    } else {
      snprintf(v.text, sizeof v.text, "-");
    }
  }
}

// ------------------------------------------------------------------ EXP ports

void Controller::pedal(int p, int value) {
  const ExpConfig& e = cfg_->exp[p];
  switch (e.target) {
    case PEDAL_AMPERO_EXP1:
    case PEDAL_AMPERO_EXP2: {
      const int sel = e.target == PEDAL_AMPERO_EXP2 ? 1 : 0;
      if (sel != ampExpSelect_) {   // CC 13: 0..63 = EXP 1, 64..127 = EXP 2
        ampero_->sendCc(13, sel ? 127 : 0);
        ampExpSelect_ = sel;
      }
      ampero_->sendCc(11, (uint8_t)value);
      break;
    }
    case PEDAL_PATCH_VOLUME: ampero_->sendCc(7, (uint8_t)((value * 100 + 63) / 127)); break;
    case PEDAL_QUICK1: ampero_->sendCc(16, (uint8_t)value); break;
    case PEDAL_QUICK2: ampero_->sendCc(18, (uint8_t)value); break;
    case PEDAL_QUICK3: ampero_->sendCc(20, (uint8_t)value); break;
    case PEDAL_DIN_CC: din_->send3(0xB0 | (e.channel & 0x0F), e.cc, (uint8_t)value); break;
    default: break;
  }
}

void Controller::poll() {
  const uint32_t now = millis();
  if (now - lastSample_ < kSampleMs) return;
  lastSample_ = now;

  for (int p = 0; p < 2; p++) {
    const ExpConfig& e = cfg_->exp[p];
    const uint8_t pin = pins_.pin[p];
    if (e.mode == EXP_PEDAL) {
      adc_select_input(pin - 26);
      const int32_t raw = adc_read();
      filt_[p] += ((raw << 4) - filt_[p]) / 8;          // low pass
      const int f = filt_[p] >> 4;
      int lo = e.rawMin, hi = e.rawMax;
      const bool reverse = lo > hi;
      if (reverse) { const int t = lo; lo = hi; hi = t; }
      if (hi - lo < 64) continue;                         // not calibrated sensibly
      // hysteresis: only react when the input moved more than a quarter step
      const int span = hi - lo;
      if (expSent_[p] >= 0 && abs(f - rawAtSent_[p]) * 512 < span) continue;
      int v = (int)((int32_t)(f - lo) * 127 / span);
      v = v < 0 ? 0 : v > 127 ? 127 : v;
      if (reverse) v = 127 - v;
      if (v == expSent_[p] || now - lastSend_[p] < kPedalMinGapMs) continue;
      expSent_[p] = v;
      rawAtSent_[p] = f;
      lastSend_[p] = now;
      pedal(p, v);
      expRevision_++;   // only the calibration page shows pedal values
    } else if (e.mode == EXP_SWITCH) {
      const bool level = gpio_get(pin);
      if (level != swLevel_[p]) {
        swLevel_[p] = level;
        swChange_[p] = now;
      }
      if (now - swChange_[p] < kDebounceMs) continue;
      const bool active = swLevel_[p] != swIdle_[p];
      if (e.latching) {                                   // latching: every change is one press
        if (active != swPressed_[p]) {
          swPressed_[p] = active;
          press(6 + p);
          release(6 + p);
        }
      } else if (active != swPressed_[p]) {
        swPressed_[p] = active;
        if (active) press(6 + p);
        else release(6 + p);
      }
    }
  }
}
