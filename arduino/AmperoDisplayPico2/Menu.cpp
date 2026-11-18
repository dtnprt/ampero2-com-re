#include "Menu.h"

#include <stdio.h>
#include <string.h>

#include "Categories.h"

namespace {

const char* const kModeNames[BTN_MODE_COUNT] = {"off", "Scene", "Scene toggle", "Block on/off", "Ampero FS"};
const char* const kExpModeNames[EXP_MODE_COUNT] = {"off", "Pedal", "Switch"};
const char* const kTargetNames[PEDAL_TARGET_COUNT] = {"Ampero EXP 1", "Ampero EXP 2", "Patch volume",
                                                      "Quick Para 1", "Quick Para 2", "Quick Para 3",
                                                      "MIDI DIN CC"};
const char* const kMidiTypeNames[MIDI_TYPE_COUNT] = {"off", "CC", "PC", "Note"};

constexpr int kBlockValues = kCategoryCount + 12;   // categories first, then slots 1..12

int blockIndex(uint8_t a) { return a >= BLOCK_CATEGORY ? a - BLOCK_CATEGORY : kCategoryCount + a; }
uint8_t blockValue(int idx) { return idx < kCategoryCount ? (uint8_t)(BLOCK_CATEGORY + idx) : (uint8_t)(idx - kCategoryCount); }

int wrap(int v, int n) { return ((v % n) + n) % n; }
int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

void Menu::open(int button) {
  depth_ = 0;
  changed_ = false;
  repeatRow_ = -1;
  push(PAGE_MAIN, 0);
  if (button >= 0) push(PAGE_BUTTON, (uint8_t)button);
}

void Menu::push(Page p, uint8_t arg) {
  if (depth_ < (int)(sizeof stack_ / sizeof stack_[0])) stack_[depth_++] = {p, arg};
}

int Menu::items(Item* out, uint8_t* args) const {
  int n = 0;
  auto add = [&](Item it, uint8_t a = 0) {
    out[n] = it;
    args[n] = a;
    n++;
  };
  const uint8_t arg = stack_[depth_ - 1].arg;
  switch (page()) {
    case PAGE_MAIN:
      for (int b = 0; b < 6; b++) add(I_MAIN_BUTTON, (uint8_t)b);
      add(I_MAIN_EXP, 0);
      add(I_MAIN_EXP, 1);
      add(I_MAIN_MIDI);
      add(I_MAIN_CAL);
      break;
    case PAGE_BUTTON: {
      const ButtonConfig& c = button();
      add(I_MODE);
      if (c.mode == BTN_SCENE) add(I_SCENE_A);
      if (c.mode == BTN_SCENE_TOGGLE) { add(I_SCENE_A); add(I_SCENE_B); }
      if (c.mode == BTN_BLOCK) add(I_BLOCK);
      if (c.mode == BTN_DEVICE_FS) { add(I_FS1); add(I_FS2); add(I_FS3); }
      add(I_MIDI_LINK);
      break;
    }
    case PAGE_BUTTON_MIDI: {
      const MidiAction& m = button().midi;
      add(I_MIDI_TYPE);
      if (m.type != MIDI_OFF) { add(I_MIDI_CH); add(I_MIDI_NUM); }
      if (m.type == MIDI_CC || m.type == MIDI_NOTE) add(I_MIDI_ON);
      if (m.type == MIDI_CC) add(I_MIDI_OFF);
      break;
    }
    case PAGE_EXP: {
      const ExpConfig& e = exp();
      add(I_EXP_MODE);
      if (e.mode == EXP_PEDAL) {
        add(I_EXP_TARGET);
        if (e.target == PEDAL_DIN_CC) { add(I_EXP_CH); add(I_EXP_CC); }
        add(I_EXP_CAL);
      } else if (e.mode == EXP_SWITCH) {
        add(I_EXP_FUNC, (uint8_t)(6 + arg));
        add(I_SWITCH_TYPE);
      }
      break;
    }
    case PAGE_EXP_CAL:
      add(I_CAL_VALUE); add(I_CAL_MIN); add(I_CAL_MAX); add(I_CAL_RESET);
      break;
    case PAGE_MIDI:
      add(I_R_CH); add(I_R_DIN_AMP); add(I_R_THRU); add(I_R_AMP_DIN);
      break;
    default:
      break;
  }
  return n;
}

void Menu::build(MenuView& v) const {
  v = MenuView();
  if (!depth_) return;
  const uint8_t arg = stack_[depth_ - 1].arg;
  switch (page()) {
    case PAGE_MAIN: snprintf(v.title, sizeof v.title, "Settings"); v.gridCols = 3; break;
    case PAGE_BUTTON:
      if (arg < 6) snprintf(v.title, sizeof v.title, "Button %u", arg + 1);
      else snprintf(v.title, sizeof v.title, "EXP %u as switch", arg - 5);
      break;
    case PAGE_BUTTON_MIDI:
      if (arg < 6) snprintf(v.title, sizeof v.title, "Button %u: MIDI DIN", arg + 1);
      else snprintf(v.title, sizeof v.title, "EXP %u: MIDI DIN", arg - 5);
      break;
    case PAGE_EXP: snprintf(v.title, sizeof v.title, "EXP %u", arg + 1); break;
    case PAGE_EXP_CAL: snprintf(v.title, sizeof v.title, "Calibrate EXP %u", arg + 1); break;
    case PAGE_MIDI: snprintf(v.title, sizeof v.title, "MIDI"); break;
    default: break;
  }
  Item its[12];
  uint8_t args[12];
  v.nRows = items(its, args);
  for (int i = 0; i < v.nRows; i++) row(its[i], args[i], v.rows[i]);
}

void Menu::row(Item it, uint8_t a, MenuRow& r) const {
  const uint8_t arg = stack_[depth_ - 1].arg;
  r.kind = ROW_VALUE;
  switch (it) {
    case I_MAIN_BUTTON: {
      snprintf(r.label, sizeof r.label, "Button %u", a + 1);
      TileView t;
      ctl_->tileView(a, t);
      const ButtonConfig& c = cfg_->buttons[a];
      if (c.mode == BTN_OFF) snprintf(r.value, sizeof r.value, "%s", c.midi.type ? "MIDI only" : "off");
      else snprintf(r.value, sizeof r.value, "%s", t.tag[0] ? t.tag : kModeNames[c.mode]);
      r.color = t.color;
      r.kind = ROW_LINK;
      break;
    }
    case I_MAIN_EXP: {
      const ExpConfig& e = cfg_->exp[a];
      snprintf(r.label, sizeof r.label, "EXP %u", a + 1);
      if (e.mode == EXP_PEDAL) snprintf(r.value, sizeof r.value, "%s", kTargetNames[e.target]);
      else snprintf(r.value, sizeof r.value, "%s", kExpModeNames[e.mode]);
      r.kind = ROW_LINK;
      break;
    }
    case I_MAIN_MIDI:
      snprintf(r.label, sizeof r.label, "MIDI");
      snprintf(r.value, sizeof r.value, "Channel %u", cfg_->midi.amperoChannel);
      r.kind = ROW_LINK;
      break;
    case I_MAIN_CAL:
      snprintf(r.label, sizeof r.label, "Touch");
      snprintf(r.value, sizeof r.value, "calibrate");
      r.kind = ROW_LINK;
      break;

    case I_MODE:
      snprintf(r.label, sizeof r.label, "Function");
      snprintf(r.value, sizeof r.value, "%s", kModeNames[button().mode]);
      break;
    case I_SCENE_A:
    case I_SCENE_B: {
      const uint8_t s = it == I_SCENE_A ? button().a : button().b;
      snprintf(r.label, sizeof r.label, button().mode == BTN_SCENE ? "Scene" : it == I_SCENE_A ? "Scene A" : "Scene B");
      snprintf(r.value, sizeof r.value, "%u %s", s + 1, ampero_->sceneName(s));
      break;
    }
    case I_BLOCK: {
      const uint8_t v = button().a;
      snprintf(r.label, sizeof r.label, "Block");
      if (v >= BLOCK_CATEGORY) {
        snprintf(r.value, sizeof r.value, "Type %s", categoryName(v - BLOCK_CATEGORY));
        r.color = categoryColor(v - BLOCK_CATEGORY);
      } else {
        const int cat = ampero_->slotCategory(v);
        snprintf(r.value, sizeof r.value, "%u %s %s", v + 1, categoryName(cat), ampero_->slotName(v));
        r.color = categoryColor(cat);
      }
      break;
    }
    case I_FS1:
    case I_FS2:
    case I_FS3: {
      const int fs = it - I_FS1;
      const char* t = ampero_->footswitchText(fs);
      const char* nl = strchr(t, '\n');
      snprintf(r.label, sizeof r.label, "FS%d %.*s", fs + 1, nl ? (int)(nl - t) : 18, t);
      const bool on = button().fsMask & (1 << fs);
      snprintf(r.value, sizeof r.value, "%s", on ? "on" : "off");
      r.color = on ? categoryColor(9) : 0;
      r.kind = ROW_TOGGLE;
      break;
    }
    case I_MIDI_LINK:
      snprintf(r.label, sizeof r.label, "MIDI DIN");
      ctl_->midiSummary(arg, r.value, sizeof r.value);
      r.kind = ROW_LINK;
      break;
    case I_SWITCH_TYPE:
      snprintf(r.label, sizeof r.label, "Switch type");
      snprintf(r.value, sizeof r.value, "%s", exp().latching ? "latching" : "momentary");
      break;

    case I_MIDI_TYPE:
      snprintf(r.label, sizeof r.label, "Message");
      snprintf(r.value, sizeof r.value, "%s", kMidiTypeNames[button().midi.type]);
      break;
    case I_MIDI_CH:
      snprintf(r.label, sizeof r.label, "Channel");
      snprintf(r.value, sizeof r.value, "%u", button().midi.channel + 1);
      break;
    case I_MIDI_NUM:
      snprintf(r.label, sizeof r.label, button().midi.type == MIDI_CC ? "CC number" : button().midi.type == MIDI_PC ? "Program" : "Note");
      snprintf(r.value, sizeof r.value, "%u", button().midi.number);
      break;
    case I_MIDI_ON:
      snprintf(r.label, sizeof r.label, button().midi.type == MIDI_NOTE ? "Velocity" : "Value 1 (on)");
      snprintf(r.value, sizeof r.value, "%u", button().midi.valueOn);
      break;
    case I_MIDI_OFF:
      snprintf(r.label, sizeof r.label, "Value 2 (off)");
      snprintf(r.value, sizeof r.value, "%u", button().midi.valueOff);
      break;

    case I_EXP_MODE:
      snprintf(r.label, sizeof r.label, "Connected");
      snprintf(r.value, sizeof r.value, "%s", kExpModeNames[exp().mode]);
      break;
    case I_EXP_TARGET:
      snprintf(r.label, sizeof r.label, "Controls");
      snprintf(r.value, sizeof r.value, "%s", kTargetNames[exp().target]);
      break;
    case I_EXP_CH:
      snprintf(r.label, sizeof r.label, "Channel");
      snprintf(r.value, sizeof r.value, "%u", exp().channel + 1);
      break;
    case I_EXP_CC:
      snprintf(r.label, sizeof r.label, "CC number");
      snprintf(r.value, sizeof r.value, "%u", exp().cc);
      break;
    case I_EXP_CAL: {
      snprintf(r.label, sizeof r.label, "Calibrate");
      snprintf(r.value, sizeof r.value, "%u..%u", exp().rawMin, exp().rawMax);
      r.kind = ROW_LINK;
      break;
    }
    case I_EXP_FUNC: {
      snprintf(r.label, sizeof r.label, "Function");
      snprintf(r.value, sizeof r.value, "%s", kModeNames[cfg_->buttons[a].mode]);
      r.kind = ROW_LINK;
      break;
    }

    case I_CAL_VALUE: {
      snprintf(r.label, sizeof r.label, "Now");
      const int raw = ctl_->expRaw(arg);
      if (raw < 0) snprintf(r.value, sizeof r.value, "-");
      else snprintf(r.value, sizeof r.value, "%d -> %d", raw, ctl_->expValue(arg));
      r.kind = ROW_INFO;
      break;
    }
    case I_CAL_MIN:
      snprintf(r.label, sizeof r.label, "Heel: tap here");
      snprintf(r.value, sizeof r.value, "%u", exp().rawMin);
      r.kind = ROW_TOGGLE;
      break;
    case I_CAL_MAX:
      snprintf(r.label, sizeof r.label, "Toe: tap here");
      snprintf(r.value, sizeof r.value, "%u", exp().rawMax);
      r.kind = ROW_TOGGLE;
      break;
    case I_CAL_RESET:
      snprintf(r.label, sizeof r.label, "Reset");
      snprintf(r.value, sizeof r.value, "0..4095");
      r.kind = ROW_TOGGLE;
      break;

    case I_R_CH:
      snprintf(r.label, sizeof r.label, "Ampero channel");
      snprintf(r.value, sizeof r.value, "%u", cfg_->midi.amperoChannel);
      break;
    case I_R_DIN_AMP:
    case I_R_THRU:
    case I_R_AMP_DIN: {
      const uint8_t on = it == I_R_DIN_AMP ? cfg_->midi.dinToAmpero : it == I_R_THRU ? cfg_->midi.dinThru : cfg_->midi.amperoToDin;
      snprintf(r.label, sizeof r.label, "%s", it == I_R_DIN_AMP ? "DIN in > Ampero" : it == I_R_THRU ? "DIN in > DIN out" : "Ampero > DIN out");
      snprintf(r.value, sizeof r.value, "%s", on ? "on" : "off");
      r.color = on ? categoryColor(9) : 0;
      r.kind = ROW_TOGGLE;
      break;
    }
    default:
      break;
  }
}

Menu::Result Menu::activate(Item it, uint8_t a, int d) {
  const uint8_t arg = stack_[depth_ - 1].arg;
  Result res = REDRAW;
  bool change = true;
  switch (it) {
    case I_MAIN_BUTTON: push(PAGE_BUTTON, a); change = false; break;
    case I_MAIN_EXP: push(PAGE_EXP, a); change = false; break;
    case I_MAIN_MIDI: push(PAGE_MIDI, 0); change = false; break;
    case I_MAIN_CAL: return CALIBRATE_TOUCH;

    case I_MODE: {
      ButtonConfig& c = button();
      c.mode = (uint8_t)wrap(c.mode + (d ? d : 1), BTN_MODE_COUNT);
      if (c.mode == BTN_SCENE_TOGGLE && c.a == c.b) c.b = (uint8_t)((c.a + 1) % 3);
      if (c.mode == BTN_BLOCK && c.a > 11 && c.a < BLOCK_CATEGORY) c.a = BLOCK_CATEGORY + 11;
      if (c.mode == BTN_BLOCK && c.a >= BLOCK_CATEGORY + kCategoryCount) c.a = BLOCK_CATEGORY + 11;
      if ((c.mode == BTN_SCENE || c.mode == BTN_SCENE_TOGGLE) && c.a > 2) c.a = 0;
      if (c.mode == BTN_SCENE_TOGGLE && (c.b > 2 || c.b == c.a)) c.b = (uint8_t)((c.a + 1) % 3);
      if (c.mode == BTN_DEVICE_FS && !c.fsMask) c.fsMask = 1;
      break;
    }
    case I_SCENE_A: button().a = (uint8_t)wrap(button().a + (d ? d : 1), 3); break;
    case I_SCENE_B: button().b = (uint8_t)wrap(button().b + (d ? d : 1), 3); break;
    case I_BLOCK: button().a = blockValue(wrap(blockIndex(button().a) + (d ? d : 1), kBlockValues)); break;
    case I_FS1:
    case I_FS2:
    case I_FS3: button().fsMask ^= (uint8_t)(1 << (it - I_FS1)); break;
    case I_MIDI_LINK: push(PAGE_BUTTON_MIDI, arg); change = false; break;
    case I_SWITCH_TYPE: exp().latching = !exp().latching; ctl_->applyExpConfig(); break;

    case I_MIDI_TYPE: button().midi.type = (uint8_t)wrap(button().midi.type + (d ? d : 1), MIDI_TYPE_COUNT); break;
    case I_MIDI_CH: button().midi.channel = (uint8_t)wrap(button().midi.channel + (d ? d : 1), 16); break;
    case I_MIDI_NUM: button().midi.number = (uint8_t)clamp(button().midi.number + (d ? d : 1), 0, 127); break;
    case I_MIDI_ON: button().midi.valueOn = (uint8_t)clamp(button().midi.valueOn + (d ? d : 1), 0, 127); break;
    case I_MIDI_OFF: button().midi.valueOff = (uint8_t)clamp(button().midi.valueOff + (d ? d : 1), 0, 127); break;

    case I_EXP_MODE: exp().mode = (uint8_t)wrap(exp().mode + (d ? d : 1), EXP_MODE_COUNT); ctl_->applyExpConfig(); break;
    case I_EXP_TARGET: exp().target = (uint8_t)wrap(exp().target + (d ? d : 1), PEDAL_TARGET_COUNT); ctl_->applyExpConfig(); break;
    case I_EXP_CH: exp().channel = (uint8_t)wrap(exp().channel + (d ? d : 1), 16); break;
    case I_EXP_CC: exp().cc = (uint8_t)clamp(exp().cc + (d ? d : 1), 0, 127); break;
    case I_EXP_CAL: push(PAGE_EXP_CAL, arg); change = false; break;
    case I_EXP_FUNC: push(PAGE_BUTTON, a); change = false; break;

    case I_CAL_MIN:
    case I_CAL_MAX: {
      const int raw = ctl_->expRaw(arg);
      if (raw < 0) { change = false; break; }
      if (it == I_CAL_MIN) exp().rawMin = (uint16_t)raw;
      else exp().rawMax = (uint16_t)raw;
      ctl_->applyExpConfig();
      break;
    }
    case I_CAL_RESET: exp().rawMin = 0; exp().rawMax = 4095; ctl_->applyExpConfig(); break;

    case I_R_CH: cfg_->midi.amperoChannel = (uint8_t)(wrap(cfg_->midi.amperoChannel - 1 + (d ? d : 1), 16) + 1); break;
    case I_R_DIN_AMP: cfg_->midi.dinToAmpero = !cfg_->midi.dinToAmpero; break;
    case I_R_THRU: cfg_->midi.dinThru = !cfg_->midi.dinThru; break;
    case I_R_AMP_DIN: cfg_->midi.amperoToDin = !cfg_->midi.amperoToDin; break;
    default: change = false; res = NOTHING; break;
  }
  if (change) changed_ = true;
  return res;
}

Menu::Result Menu::press(int W, int H, int x, int y) {
  repeatRow_ = -1;
  if (!depth_) return NOTHING;
  MenuView v;
  build(v);
  const MenuHit hit = menuHitTest(W, H, v, x, y);
  if (hit.row == -1) {   // back
    depth_--;
    return depth_ ? REDRAW : CLOSED;
  }
  if (hit.row < 0) return NOTHING;
  Item its[12];
  uint8_t args[12];
  items(its, args);
  const Item it = its[hit.row];
  const int delta = v.rows[hit.row].kind == ROW_VALUE ? (hit.zone ? hit.zone : 1) : 0;
  const Result r = activate(it, args[hit.row], delta);
  if (r == REDRAW && delta && (it == I_MIDI_NUM || it == I_MIDI_ON || it == I_MIDI_OFF || it == I_EXP_CC ||
                               it == I_BLOCK)) {
    repeatRow_ = hit.row;   // numbers: keep counting while held
    repeatDelta_ = delta;
    repeatItem_ = it;
    repeatArg_ = args[hit.row];
  }
  return r;
}

Menu::Result Menu::repeat() {
  if (repeatRow_ < 0 || !depth_) return NOTHING;
  return activate(repeatItem_, repeatArg_, repeatDelta_);
}
