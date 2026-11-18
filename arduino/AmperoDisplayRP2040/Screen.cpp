#include "Screen.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t C_BG = rgb565(0, 0, 0);
constexpr uint16_t C_TILE = rgb565(38, 42, 50);
constexpr uint16_t C_TILE_BORDER = rgb565(70, 76, 88);
constexpr uint16_t C_TEXT = rgb565(236, 238, 242);
constexpr uint16_t C_MUTED = rgb565(150, 156, 168);
constexpr uint16_t C_ACCENT = rgb565(245, 165, 36);
constexpr uint16_t C_ACCENT_TEXT = rgb565(17, 17, 17);
constexpr uint16_t C_OK = rgb565(60, 200, 90);
constexpr uint16_t C_WARN = rgb565(220, 60, 60);

const GFXfont* const kBig[] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
const GFXfont* const kMid[] = {&FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
const GFXfont* const kSmall[] = {&FreeSans9pt7b};

Adafruit_GFX* g;

void textSize(const char* s, const GFXfont* f, int16_t& w, int16_t& h, int16_t& yOff) {
  int16_t x1, y1;
  uint16_t tw, th;
  g->setFont(f);
  g->setTextSize(1);
  g->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  w = (int16_t)tw;
  h = (int16_t)th;
  yOff = y1;
}

/** Draws s centered in the box with the largest font that fits; shortens with "..." if needed. */
void drawFit(const char* s, int x, int y, int w, int h, uint16_t color, const GFXfont* const* fonts, int nFonts) {
  char buf[40];
  snprintf(buf, sizeof buf, "%s", s);
  int16_t tw, th, yo;
  const GFXfont* f = fonts[nFonts - 1];
  for (int i = 0; i < nFonts; i++) {
    textSize(buf, fonts[i], tw, th, yo);
    if (tw <= w && th <= h) { f = fonts[i]; break; }
  }
  textSize(buf, f, tw, th, yo);
  while (tw > w && strlen(buf) > 3) {
    size_t L = strlen(buf);
    buf[L - 4] = '.'; buf[L - 3] = '.'; buf[L - 2] = '.'; buf[L - 1] = 0;
    textSize(buf, f, tw, th, yo);
  }
  g->setFont(f);
  g->setTextColor(color);
  g->setCursor(x + (w - tw) / 2, y + (h - th) / 2 - yo);
  g->print(buf);
}

/** Largest font in which every one of the n texts fits into w x h (so the tiles look alike). */
const GFXfont* commonFont(const char* const* texts, int n, int w, int h, const GFXfont* const* fonts, int nFonts) {
  for (int i = 0; i < nFonts; i++) {
    bool all = true;
    for (int k = 0; k < n && all; k++) {
      int16_t tw, th, yo;
      textSize(texts[k], fonts[i], tw, th, yo);
      all = tw <= w && th <= h;
    }
    if (all) return fonts[i];
  }
  return fonts[nFonts - 1];
}

void drawLeft(const char* s, int x, int baseline, uint16_t color) {
  g->setFont(&FreeSans9pt7b);
  g->setTextColor(color);
  g->setCursor(x, baseline);
  g->print(s);
}

void drawStatusLine(const char* left, bool usb) {
  const int W = g->width();
  drawLeft(left, 8, 18, C_MUTED);
  g->fillCircle(W - 66, 12, 5, usb ? C_OK : C_WARN);
  drawLeft(usb ? "USB" : "USB --", W - 56, 18, C_MUTED);
}

}  // namespace

void renderScreen(Adafruit_GFX& gfx, const ScreenState& s) {
  g = &gfx;
  gfx.setTextWrap(false);
  const int W = gfx.width(), H = gfx.height();
  gfx.fillScreen(C_BG);

  if (!s.usbConnected || !s.online) {
    drawStatusLine("AMPERO II", s.usbConnected);
    drawFit("Ampero nicht verbunden", 0, H / 2 - 40, W, 40, C_TEXT, kMid, 3);
    drawFit("USB-Kabel zum Ampero stecken", 0, H / 2 + 8, W, 24, C_MUTED, kSmall, 1);
    return;
  }
  if (!s.hasPreset) {
    drawStatusLine("AMPERO II", true);
    drawFit("Lese Preset ...", 0, H / 2 - 25, W, 50, C_TEXT, kMid, 3);
    return;
  }

  char head[32];
  snprintf(head, sizeof head, "PRESET %03d", s.presetIndex);
  drawStatusLine(head, true);

  const bool landscape = W > H;
  const int margin = 8, gap = 8;
  const int nameTop = 26;
  const int nameH = landscape ? H * 11 / 32 : H * 9 / 32;   // 320x240: 82 px, 240x320: 90 px
  drawFit(s.presetName, margin, nameTop, W - 2 * margin, nameH, C_TEXT, kBig, 4);

  const int tilesTop = nameTop + nameH + 6;
  const int tilesH = H - tilesTop - margin;
  const int labelH = 20;
  const char* names[3];
  for (int i = 0; i < 3; i++) names[i] = s.sceneNames[i] && s.sceneNames[i][0] ? s.sceneNames[i] : "-";
  // text area of one tile (same formulas as below)
  const int areaW = landscape ? (W - 2 * margin - 2 * gap) / 3 - 8 : W - 2 * margin - 92;
  const int areaH = landscape ? tilesH - labelH - 10 : (tilesH - 2 * gap) / 3 - 4;
  const GFXfont* const nameFont[] = {commonFont(names, 3, areaW, areaH, kMid, 3)};

  for (int i = 0; i < 3; i++) {
    int x, y, w, h;
    if (landscape) {
      w = (W - 2 * margin - 2 * gap) / 3;
      h = tilesH;
      x = margin + i * (w + gap);
      y = tilesTop;
    } else {
      w = W - 2 * margin;
      h = (tilesH - 2 * gap) / 3;
      x = margin;
      y = tilesTop + i * (h + gap);
    }
    const bool active = i == s.activeScene;
    gfx.fillRoundRect(x, y, w, h, 8, active ? C_ACCENT : C_TILE);
    gfx.drawRoundRect(x, y, w, h, 8, active ? C_ACCENT : C_TILE_BORDER);
    if (active) gfx.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 6, C_ACCENT_TEXT);

    char label[16];
    snprintf(label, sizeof label, "SZENE %d", i + 1);
    if (landscape) {
      drawFit(label, x, y + 4, w, labelH, active ? C_ACCENT_TEXT : C_MUTED, kSmall, 1);
      drawFit(names[i], x + 4, y + labelH + 6, w - 8, h - labelH - 10, active ? C_ACCENT_TEXT : C_TEXT, nameFont, 1);
    } else {   // portrait: label left, name right
      drawFit(label, x + 4, y, 80, h, active ? C_ACCENT_TEXT : C_MUTED, kSmall, 1);
      drawFit(names[i], x + 84, y + 2, w - 92, h - 4, active ? C_ACCENT_TEXT : C_TEXT, nameFont, 1);
    }
  }
}
