// Renders the RP2040/HY28B screen on a PC through the same StripCanvas (strip by strip)
// and writes raw RGB565 frames; convert with make_png.py.
#include <cstdio>
#include <vector>
#include "../../Screen.h"
#include "../../StripCanvas.h"

static void render(int W, int H, const ScreenState& s, const char* name) {
  StripCanvas c(W, H, 30);
  std::vector<uint16_t> frame((size_t)W * H);
  for (int y = 0; y < H; y += 30) {
    c.setStrip(y);
    renderScreen(c, s);
    for (int r = 0; r < c.validRows(); r++)
      for (int x = 0; x < W; x++) frame[(size_t)(y + r) * W + x] = c.buffer()[r * W + x];
  }
  FILE* f = fopen(name, "wb");
  uint16_t wh[2] = {(uint16_t)W, (uint16_t)H};
  fwrite(wh, 2, 2, f);
  fwrite(frame.data(), 2, frame.size(), f);
  fclose(f);
}

int main() {
  ScreenState s;
  s.usbConnected = s.online = s.hasPreset = true;
  s.presetIndex = 6;
  s.presetName = "Juicy Fusion";
  s.sceneNames[0] = "Scene 1"; s.sceneNames[1] = "Scene 2"; s.sceneNames[2] = "Scene 3";
  s.activeScene = 1;
  render(320, 240, s, "r1_juicy.rgb565");
  s.presetIndex = 75;
  s.presetName = "ABCTESTHIJKLMNOP";
  s.sceneNames[0] = "Narf"; s.sceneNames[1] = "TEST"; s.sceneNames[2] = "WWWWWWW";
  s.activeScene = 2;
  render(320, 240, s, "r2_long.rgb565");
  render(240, 320, s, "r4_portrait.rgb565");
  ScreenState off;
  render(320, 240, off, "r3_offline.rgb565");
  return 0;
}
