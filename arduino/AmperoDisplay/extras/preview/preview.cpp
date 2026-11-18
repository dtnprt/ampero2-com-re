// Renders the AmperoDisplay screen on a PC and writes raw RGB565 frames (see make_png.py).
//   cl /EHsc /std:c++17 /DARDUINO=100 /I. /I<Adafruit_GFX_Library> preview.cpp ..\..\Screen.cpp <Adafruit_GFX_Library>\Adafruit_GFX.cpp
#include <cstdio>
#include "../../Screen.h"

static void save(GFXcanvas16& c, const char* name) {
  FILE* f = fopen(name, "wb");
  uint16_t wh[2] = {(uint16_t)c.width(), (uint16_t)c.height()};
  fwrite(wh, 2, 2, f);
  fwrite(c.getBuffer(), 2, (size_t)c.width() * c.height(), f);
  fclose(f);
}

int main() {
  GFXcanvas16 land(480, 320), port(320, 480);
  ScreenState s;
  s.usbConnected = s.online = s.hasPreset = true;
  s.presetIndex = 6;
  s.presetName = "Juicy Fusion";
  s.sceneNames[0] = "Scene 1"; s.sceneNames[1] = "Scene 2"; s.sceneNames[2] = "Scene 3";
  s.activeScene = 1;
  renderScreen(land, s); save(land, "p1_juicy.rgb565");

  s.presetIndex = 75;
  s.presetName = "ABCTESTHIJKLMNOP";
  s.sceneNames[0] = "Narf"; s.sceneNames[1] = "TEST"; s.sceneNames[2] = "WWWWWWW";
  s.activeScene = 2;
  renderScreen(land, s); save(land, "p2_long.rgb565");
  renderScreen(port, s); save(port, "p4_portrait.rgb565");

  ScreenState off;
  renderScreen(land, off); save(land, "p3_offline.rgb565");
  return 0;
}
