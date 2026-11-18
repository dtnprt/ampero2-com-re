#pragma once
/*
 * Settings menu on the touchscreen: function of the six tiles, EXP ports (pedal/footswitch,
 * calibration), MIDI-DIN messages and routing. Only ASCII texts (the GFX fonts have no umlauts).
 */
#include "AmperoClient.h"
#include "Config.h"
#include "Controller.h"
#include "Screen.h"

class Menu {
 public:
  enum Page : uint8_t { PAGE_MAIN, PAGE_BUTTON, PAGE_BUTTON_MIDI, PAGE_EXP, PAGE_EXP_CAL, PAGE_MIDI, PAGE_NONE };
  enum Result : uint8_t { NOTHING, REDRAW, CLOSED };

  void begin(AppConfig* cfg, AmperoClient* ampero, Controller* ctl) {
    cfg_ = cfg;
    ampero_ = ampero;
    ctl_ = ctl;
  }
  bool isOpen() const { return depth_ > 0; }
  /** Opens the main menu, or directly the page of button b (0..5). */
  void open(int button = -1);
  Page page() const { return depth_ ? stack_[depth_ - 1].page : PAGE_NONE; }
  /** Settings changed since the menu was opened (save when it closes). */
  bool changed() const { return changed_; }

  void build(MenuView& v) const;
  /** Touch down at (x, y). */
  Result press(int W, int H, int x, int y);
  /** The current press may repeat while held (value arrows). */
  bool repeatable() const { return repeatRow_ >= 0; }
  Result repeat();

 private:
  struct Level {
    Page page;
    uint8_t arg;   // button 0..7 / EXP port 0..1
  };
  enum Item : uint8_t {
    I_NONE,
    I_MAIN_BUTTON,   // + arg in itemArg_
    I_MAIN_EXP,
    I_MAIN_MIDI,
    I_MODE, I_SCENE_A, I_SCENE_B, I_BLOCK, I_FS1, I_FS2, I_FS3, I_MIDI_LINK, I_SWITCH_TYPE,
    I_MIDI_TYPE, I_MIDI_CH, I_MIDI_NUM, I_MIDI_ON, I_MIDI_OFF,
    I_EXP_MODE, I_EXP_TARGET, I_EXP_CH, I_EXP_CC, I_EXP_CAL, I_EXP_FUNC,
    I_CAL_VALUE, I_CAL_MIN, I_CAL_MAX, I_CAL_RESET,
    I_R_CH, I_R_DIN_AMP, I_R_THRU, I_R_AMP_DIN,
  };

  int items(Item* out, uint8_t* args) const;
  void row(Item it, uint8_t arg, MenuRow& r) const;
  Result activate(Item it, uint8_t arg, int delta);
  void push(Page p, uint8_t arg);
  ButtonConfig& button() const { return cfg_->buttons[stack_[depth_ - 1].arg]; }
  ExpConfig& exp() const { return cfg_->exp[stack_[depth_ - 1].arg]; }

  AppConfig* cfg_ = nullptr;
  AmperoClient* ampero_ = nullptr;
  Controller* ctl_ = nullptr;
  Level stack_[5];
  int depth_ = 0;
  bool changed_ = false;
  int repeatRow_ = -1, repeatDelta_ = 0;
  Item repeatItem_ = I_NONE;
  uint8_t repeatArg_ = 0;
};
