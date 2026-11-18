// PC test: 3-point touch calibration for several panel orientations, tile/menu hit testing,
// MIDI parser and settings storage.
//   cl /EHsc /std:c++17 /DARDUINO=100 /I. /I<Adafruit_GFX_Library> touch_test.cpp ..\..\Xpt2046.cpp ..\..\Screen.cpp
//      ..\..\Config.cpp <GFX>\Adafruit_GFX.cpp
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <EEPROM.h>
#include "../../Config.h"
#include "../../MidiParser.h"
#include "../../Screen.h"
#include "../../Xpt2046.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

int main() {
  const int W = 320, H = 240;
  const int px[3] = {30, W - 30, W / 2}, py[3] = {30, H / 2, H - 30};   // as in calibrateTouch()

  // raw = f(screen): plausible XPT2046 mappings (scale ~11-15 counts/pixel, offsets, swapped/mirrored axes)
  struct Case { const char* name; std::function<void(double, double, double&, double&)> f; };
  Case cases[] = {
      {"normal", [](double x, double y, double& rx, double& ry) { rx = 200 + x * 11.5; ry = 300 + y * 14.6; }},
      {"x gespiegelt", [](double x, double y, double& rx, double& ry) { rx = 3900 - x * 11.5; ry = 300 + y * 14.6; }},
      {"Achsen getauscht", [](double x, double y, double& rx, double& ry) { rx = 250 + y * 15.0; ry = 3850 - x * 11.2; }},
      {"getauscht + verrauscht", [](double x, double y, double& rx, double& ry) {
         rx = 250 + y * 15.0 + 0.002 * x * y; ry = 3850 - x * 11.2 + ((int)(x * 7 + y * 3) % 9 - 4); }},
  };
  for (auto& c : cases) {
    int rx[3], ry[3];
    for (int i = 0; i < 3; i++) { double a, b; c.f(px[i], py[i], a, b); rx[i] = (int)a; ry[i] = (int)b; }
    TouchCalibration cal{};
    CHECK(Xpt2046::solve(px, py, rx, ry, cal));
    Xpt2046 t;
    t.setCalibration(cal);
    int maxErr = 0;
    for (int y = 10; y < H; y += 20)
      for (int x = 10; x < W; x += 20) {
        double a, b; c.f(x, y, a, b);
        int sx, sy; t.map((int)a, (int)b, sx, sy);
        maxErr = std::max(maxErr, std::max(std::abs(sx - x), std::abs(sy - y)));
      }
    // tap on every tile centre must hit that tile
    bool tilesOk = true;
    for (int i = 0; i < 6; i++) {
      int x, y, w, h; tileRect(W, H, i, x, y, w, h);
      double a, b; c.f(x + w / 2, y + h / 2, a, b);
      int sx, sy; t.map((int)a, (int)b, sx, sy);
      tilesOk &= tileAt(W, H, sx, sy) == i;
    }
    std::printf("%-24s max. Abweichung %2d px, Kacheln %s\n", c.name, maxErr, tilesOk ? "ok" : "FALSCH");
    CHECK(maxErr <= 6);
    CHECK(tilesOk);
  }

  // hit testing: outside the tiles nothing is selected, both orientations; all six tiles equal
  CHECK(tileAt(W, H, 160, 40) == -1);              // preset name area
  CHECK(tileAt(240, 320, 120, 40) == -1);
  CHECK(statusBarAt(W, H, 300, 10) && !statusBarAt(W, H, 300, 60));
  for (int o = 0; o < 2; o++) {
    const int ww = o ? 240 : 320, hh = o ? 320 : 240;
    int x0, y0, w0, h0;
    tileRect(ww, hh, 0, x0, y0, w0, h0);
    for (int i = 0; i < 6; i++) {
      int x, y, w, h;
      tileRect(ww, hh, i, x, y, w, h);
      CHECK(w == w0 && h == h0);
      CHECK(x >= 0 && y >= 30 && x + w <= ww && y + h <= hh);
      CHECK(tileAt(ww, hh, x + w / 2, y + h / 2) == i);
    }
  }

  // menu hit test: back button, arrows, rows
  MenuView mv;
  mv.nRows = 3;
  mv.rows[0].kind = ROW_VALUE;
  mv.rows[1].kind = ROW_TOGGLE;
  mv.rows[2].kind = ROW_LINK;
  CHECK(menuHitTest(W, H, mv, 20, 12).row == -1);
  MenuHit mh = menuHitTest(W, H, mv, W - 20, 47);
  CHECK(mh.row == 0 && mh.zone == 1);
  mh = menuHitTest(W, H, mv, 40, 47);
  CHECK(mh.row == 0 && mh.zone == 0);
  mh = menuHitTest(W, H, mv, W - 180, 47);
  CHECK(mh.row == 0 && mh.zone == -1);
  CHECK(menuHitTest(W, H, mv, 100, 80).row == 1);
  CHECK(menuHitTest(W, H, mv, 100, 230).row == -2);

  // MIDI parser: running status, realtime inside a message and SysEx, system common, overflow
  {
    MidiParser<16> p;
    std::vector<std::string> got;
    auto feed = [&](std::vector<uint8_t> bytes) {
      for (uint8_t b : bytes)
        if (p.feed(b)) {
          char s[80] = "";
          for (size_t i = 0; i < p.len(); i++) snprintf(s + strlen(s), sizeof s - strlen(s), "%02X", p.msg()[i]);
          got.push_back(s);
        }
    };
    feed({0xB0, 0x07, 0x64, 0x08, 0x10, 0xF8, 0x0A, 0x20});   // CC, running status, clock in between
    feed({0xC2, 0x05, 0x06});                                  // PC + running status
    feed({0xF0, 0x21, 0xF8, 0x25, 0xF7});                      // SysEx with clock inside
    feed({0xF3, 0x02, 0xF6});                                  // song select, tune request
    feed({0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0xF7});   // too long: dropped
    feed({0x90, 0x3C, 0x40});
    const std::vector<std::string> want = {"B00764", "B00810", "F8", "B00A20", "C205", "C206", "F8",
                                           "F02125F7", "F302", "F6", "903C40"};
    CHECK(got == want);
    if (got != want)
      for (auto& s : got) std::printf("  %s\n", s.c_str());
  }

  // settings: defaults, roundtrip, corrupted data -> defaults
  {
    AppConfig a, b;
    configDefaults(a);
    CHECK(a.buttons[0].mode == BTN_SCENE && a.buttons[2].a == 2 && a.buttons[5].mode == BTN_DEVICE_FS &&
          a.buttons[5].fsMask == 4 && a.midi.amperoChannel == 1);
    a.buttons[1].mode = BTN_BLOCK;
    a.buttons[1].a = BLOCK_CATEGORY + 11;
    a.exp[1].mode = EXP_PEDAL;
    a.exp[1].rawMax = 3000;
    configSave(a);
    CHECK(configLoad(b) && b.buttons[1].a == BLOCK_CATEGORY + 11 && b.exp[1].rawMax == 3000);
    EEPROM.mem[64 + 20] ^= 0xFF;
    CHECK(!configLoad(b) && b.buttons[1].mode == BTN_SCENE);
    CHECK(64 + sizeof(AppConfig) <= (size_t)Xpt2046::kEepromSize);
  }

  // collinear points are rejected
  int cx[3] = {10, 20, 30}, cy[3] = {10, 20, 30}, crx[3] = {100, 200, 300}, cry[3] = {100, 200, 300};
  TouchCalibration bad{};
  CHECK(!Xpt2046::solve(cx, cy, crx, cry, bad));

  // EEPROM roundtrip
  TouchCalibration s{1, 2, 3, 4, 5, 6, TouchCalibration::kMagic}, l{};
  Xpt2046::saveCalibration(s);
  CHECK(Xpt2046::loadCalibration(l) && l.c == 3 && l.valid());

  std::printf(fails ? "\n%d FEHLER\n" : "\nalle Tests ok\n", fails);
  return fails ? 1 : 0;
}
