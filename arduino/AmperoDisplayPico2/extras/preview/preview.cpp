// Renders the Pico 2/HY28B screens on a PC through the same StripCanvas (strip by strip)
// and writes raw RGB565 frames; convert with make_png.py.
// Argument: directory with preset dumps from the capture (tools/ampero_sysex.py,
// "NNN_in_01000000.bin" = u32 index + TLV); they go through AmperoClient's real preset parser and
// the Controller/Menu code of the sketch.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#define private public   // preview only: feed dumps straight into the parser (same mangling in both)
#include "../../AmperoClient.cpp"
#include "../../Menu.cpp"
#undef private
#include "../../Config.h"
#include "../../Controller.h"
#include "../../Screen.h"
#include "../../StripCanvas.h"

static void write(int W, int H, const std::function<void(Adafruit_GFX&)>& draw, const char* name) {
  StripCanvas c(W, H, 30);
  std::vector<uint16_t> frame((size_t)W * H);
  for (int y = 0; y < H; y += 30) {
    c.setStrip(y);
    draw(c);
    for (int r = 0; r < c.validRows(); r++)
      for (int x = 0; x < W; x++) frame[(size_t)(y + r) * W + x] = c.buffer()[r * W + x];
  }
  FILE* f = fopen(name, "wb");
  uint16_t wh[2] = {(uint16_t)W, (uint16_t)H};
  fwrite(wh, 2, 2, f);
  fwrite(frame.data(), 2, frame.size(), f);
  fclose(f);
}

static AmperoClient client;
static AppConfig cfg;
static MidiDin din;
static Controller ctl;
static Menu menu;

static bool sendNothing(const uint8_t*, size_t) { return true; }

static bool loadDump(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> d(16384);
  d.resize(fread(d.data(), 1, d.size(), f));
  fclose(f);
  if (d.size() < 8 || !client.parsePreset(d.data() + 4, d.size() - 4)) return false;
  client.presetIndex_ = d[0] | (d[1] << 8);
  client.online_ = true;
  client.activeScene_ = 1;
  return true;
}

static void screen(int W, int H, const char* name) {
  ScreenState s;
  s.usbConnected = s.online = s.hasPreset = true;
  s.presetIndex = client.presetIndex();
  s.presetName = client.presetName();
  s.sceneName = client.sceneName(client.activeScene());
  for (int i = 0; i < 6; i++) ctl.tileView(i, s.tiles[i]);
  printf("%s: \"%s\"\n", name, s.presetName);
  for (int i = 0; i < 6; i++) {
    std::string t = s.tiles[i].text;
    for (char& c : t) if (c == '\n') c = '|';
    printf("  %d [%s] %s (%d)\n", i + 1, s.tiles[i].tag, t.c_str(), s.tiles[i].state);
  }
  write(W, H, [&](Adafruit_GFX& g) { renderScreen(g, s); }, name);
}

static void menuPage(const char* name) {
  MenuView v;
  menu.build(v);
  write(320, 240, [&](Adafruit_GFX& g) { renderMenu(g, v); }, name);
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  configDefaults(cfg);
  client.begin(sendNothing);
  ctl.begin(&cfg, &client, &din, {{26, 27}});
  menu.begin(&cfg, &client, &ctl);

  // default layout (scenes + device footswitches) with "Basic Clean" and "Le Chug"
  if (loadDump(dir + "/043_in_01000000.bin")) screen(320, 240, "r1_default.rgb565");
  if (loadDump(dir + "/009_in_01000000.bin")) {
    screen(320, 240, "r2_chug.rgb565");
    screen(240, 320, "r4_portrait.rgb565");
  }
  // custom layout: toggle scenes, blocks by type and slot, two device footswitches, MIDI only
  if (loadDump(dir + "/050_in_01000000.bin")) {
    cfg.buttons[0] = {BTN_SCENE_TOGGLE, 0, 1, 0, {}, {}};
    cfg.buttons[1] = {BTN_BLOCK, BLOCK_CATEGORY + 11, 0, 0, {}, {}};   // RVB
    cfg.buttons[2] = {BTN_BLOCK, BLOCK_CATEGORY + 15, 0, 0, {}, {}};   // VOL
    cfg.buttons[3] = {BTN_BLOCK, 3, 0, 0, {}, {}};                      // slot 4
    cfg.buttons[4] = {BTN_DEVICE_FS, 0, 0, 3, {}, {}};                  // FS1 + FS2
    cfg.buttons[5] = {BTN_OFF, 0, 0, 0, {MIDI_CC, 0, 64, 127, 0}, {}};
    screen(320, 240, "r6_custom.rgb565");
  }
  // menu pages
  menu.open();
  menuPage("m1_main.rgb565");
  menu.open(1);
  menuPage("m2_button_block.rgb565");
  menu.open(4);
  menuPage("m3_button_fs.rgb565");
  menu.push(Menu::PAGE_BUTTON_MIDI, 5);
  menuPage("m4_midi_din.rgb565");
  cfg.exp[0].mode = EXP_PEDAL;
  cfg.exp[0].target = PEDAL_DIN_CC;
  menu.open();
  menu.push(Menu::PAGE_EXP, 0);
  menuPage("m5_exp.rgb565");
  menu.push(Menu::PAGE_EXP_CAL, 0);
  menuPage("m6_exp_cal.rgb565");
  menu.open();
  menu.push(Menu::PAGE_MIDI, 0);
  menuPage("m7_routing.rgb565");

  ScreenState off;
  write(320, 240, [&](Adafruit_GFX& g) { renderScreen(g, off); }, "r3_offline.rgb565");
  write(320, 240, [&](Adafruit_GFX& g) { renderCalibration(g, 0, 30, 30, "Tap the centre of the cross (stylus)"); },
        "r5_calibration.rgb565");
  return 0;
}
