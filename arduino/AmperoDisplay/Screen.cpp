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

const GFXfont* const kBig[] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b};
const GFXfont* const kMid[] = {&FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
const GFXfont* const kSmall[] = {&FreeSans9pt7b};

GFXcanvas16* g;

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

void drawLeft(const char* s, int x, int baseline, uint16_t color) {
  g->setFont(&FreeSans9pt7b);
  g->setTextColor(color);
  g->setCursor(x, baseline);
  g->print(s);
}

void drawStatusLine(const char* left, bool usb) {
  const int W = g->width();
  drawLeft(left, 12, 24, C_MUTED);
  g->fillCircle(W - 74, 18, 6, usb ? C_OK : C_WARN);
  drawLeft(usb ? "USB" : "USB --", W - 62, 24, C_MUTED);
}

}  // namespace

void renderScreen(GFXcanvas16& canvas, const ScreenState& s) {
  g = &canvas;
  canvas.setTextWrap(false);
  const int W = canvas.width(), H = canvas.height();
  canvas.fillScreen(C_BG);

  if (!s.usbConnected || !s.online) {
    drawStatusLine("AMPERO II", s.usbConnected);
    drawFit("Ampero nicht verbunden", 0, H / 2 - 50, W, 50, C_TEXT, kMid, 3);
    drawFit("USB-OTG-Kabel zum Ampero stecken", 0, H / 2 + 10, W, 30, C_MUTED, kSmall, 1);
    return;
  }
  if (!s.hasPreset) {
    drawStatusLine("AMPERO II", true);
    drawFit("Lese Preset ...", 0, H / 2 - 30, W, 60, C_TEXT, kMid, 3);
    return;
  }

  char head[32];
  snprintf(head, sizeof head, "PRESET %03d", s.presetIndex);
  drawStatusLine(head, true);

  const bool landscape = W > H;
  const int nameTop = 36, nameH = landscape ? 110 : 150;
  drawFit(s.presetName, 10, nameTop, W - 20, nameH, C_TEXT, kBig, 3);

  const int margin = 10, gap = 10;
  const int tilesTop = nameTop + nameH + 8;
  const int tilesH = H - tilesTop - margin;
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
    canvas.fillRoundRect(x, y, w, h, 10, active ? C_ACCENT : C_TILE);
    canvas.drawRoundRect(x, y, w, h, 10, active ? C_ACCENT : C_TILE_BORDER);
    if (active) canvas.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 8, C_ACCENT_TEXT);

    char label[16];
    snprintf(label, sizeof label, "SZENE %d", i + 1);
    drawFit(label, x, y + 6, w, 22, active ? C_ACCENT_TEXT : C_MUTED, kSmall, 1);
    const char* name = s.sceneNames[i] && s.sceneNames[i][0] ? s.sceneNames[i] : "-";
    drawFit(name, x + 6, y + 30, w - 12, h - 36, active ? C_ACCENT_TEXT : C_TEXT, kMid, 3);
  }
}
