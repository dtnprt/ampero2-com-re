#include "Screen.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <stdio.h>
#include <stdlib.h>
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
constexpr uint16_t C_DARK_TEXT = rgb565(17, 17, 17);
constexpr uint16_t C_OK = rgb565(60, 200, 90);
constexpr uint16_t C_WARN = rgb565(220, 60, 60);
constexpr uint16_t C_ROW = rgb565(28, 31, 37);

const GFXfont* const kBig[] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
const GFXfont* const kMid[] = {&FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
const GFXfont* const kSmall[] = {&FreeSans9pt7b};
const GFXfont* const kOneLine[] = {&FreeSansBold12pt7b, &FreeSansBold9pt7b, &FreeSans9pt7b};
const GFXfont* const kTwoLines[] = {&FreeSansBold9pt7b, &FreeSans9pt7b};
const GFXfont* const kRow[] = {&FreeSansBold9pt7b, &FreeSans9pt7b};

constexpr int kNameTop = 26;
constexpr int kMargin = 6, kGap = 6;
constexpr int kTagH = 11;       // small label in the classic 6x8 font
constexpr int kMenuTop = 32, kMenuRowH = 31, kMenuGap = 3;

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

/**
 * Draws s in the box with the largest font that fits; shortens with "..." if needed.
 * align: 0 = centered, -1 = left, +1 = right (always vertically centered).
 */
void drawFit(const char* s, int x, int y, int w, int h, uint16_t color, const GFXfont* const* fonts, int nFonts,
             int align = 0) {
  char buf[48];
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
  const int cx = align < 0 ? x : align > 0 ? x + w - tw : x + (w - tw) / 2;
  g->setCursor(cx, y + (h - th) / 2 - yo);
  g->print(buf);
}

/** Classic 6x8 font, for the small tile labels. */
void drawTag(const char* s, int x, int y, uint16_t color) {
  g->setFont(nullptr);
  g->setTextSize(1);
  g->setTextColor(color);
  g->setCursor(x, y);
  g->print(s);
}

void drawLeft(const char* s, int x, int baseline, uint16_t color) {
  g->setFont(&FreeSans9pt7b);
  g->setTextColor(color);
  g->setCursor(x, baseline);
  g->print(s);
}

void drawMenuIcon(int x, int y, uint16_t c) {
  for (int i = 0; i < 3; i++) g->fillRect(x, y + i * 6, 20, 3, c);
}

/** Preset number left (white), small USB indicator, menu icon at the far right. */
void drawStatusLine(const char* left, bool usb) {
  const int W = g->width();
  drawLeft(left, 8, 18, C_TEXT);
  drawMenuIcon(W - 26, 5, C_TEXT);
  g->fillCircle(W - 54, 12, 3, usb ? C_OK : C_WARN);
  drawTag("USB", W - 48, 9, C_MUTED);
}

int nameHeight(int W, int H) { return W > H ? 72 : 84; }   // preset name + active scene below
constexpr int kSceneLineH = 24;

void drawCross(int x, int y, uint16_t c) {
  g->drawFastHLine(x - 14, y, 29, c);
  g->drawFastVLine(x, y - 14, 29, c);
  g->drawCircle(x, y, 8, c);
}

/** Text of one tile as at most two lines. */
struct Lines {
  char buf[64];
  char second[40];
  const char* line[2];
  int n;
};

/**
 * Splits '\n'-separated text into at most two lines (more: the 2nd line ends with "+n").
 * A single line that does not fit into w is wrapped at the space closest to the middle.
 */
void layoutLines(const char* text, int w, Lines& L) {
  snprintf(L.buf, sizeof L.buf, "%s", text);
  char* lines[8];
  int n = 0;
  for (char* p = L.buf; n < 8;) {
    lines[n++] = p;
    char* nl = strchr(p, '\n');
    if (!nl) break;
    *nl = 0;
    p = nl + 1;
  }
  int16_t tw, th, yo;
  textSize(lines[0], kOneLine[2], tw, th, yo);
  if (n == 1 && tw > w) {
    char* best = nullptr;
    const int len = (int)strlen(lines[0]);
    for (char* sp = strchr(lines[0], ' '); sp; sp = strchr(sp + 1, ' '))
      if (!best || abs((int)(sp - lines[0]) - len / 2) < abs((int)(best - lines[0]) - len / 2)) best = sp;
    if (best) {
      *best = 0;
      lines[n++] = best + 1;
    }
  }
  L.line[0] = lines[0];
  L.n = n > 2 ? 2 : n;
  if (n >= 2) {
    if (n > 2) snprintf(L.second, sizeof L.second, "%s +%d", lines[1], n - 2);
    else snprintf(L.second, sizeof L.second, "%s", lines[1]);
    L.line[1] = L.second;
  }
}

/** Largest two-line font in which both lines fit into w x h each. */
const GFXfont* commonFont2(const char* const* lines, int w, int h) {
  for (const GFXfont* f : kTwoLines) {
    bool all = true;
    for (int k = 0; k < 2 && all; k++) {
      int16_t tw, th, yo;
      textSize(lines[k], f, tw, th, yo);
      all = tw <= w && th <= h;
    }
    if (all) return f;
  }
  return kTwoLines[1];
}

void drawTile(const TileView& t, int x, int y, int w, int h) {
  const uint16_t color = t.color ? t.color : C_TILE_BORDER;
  uint16_t text, tag;
  if (t.state == 1) {          // on: filled with the effect colour
    g->fillRoundRect(x, y, w, h, 7, color);
    text = tag = C_DARK_TEXT;
  } else if (t.state == 0) {   // off: dark, coloured frame
    g->fillRoundRect(x, y, w, h, 7, C_TILE);
    g->drawRoundRect(x, y, w, h, 7, color);
    g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, color);
    text = C_TEXT;
    tag = color;
  } else {                     // no state
    g->fillRoundRect(x, y, w, h, 7, C_TILE);
    g->drawRoundRect(x, y, w, h, 7, C_TILE_BORDER);
    text = C_MUTED;
    tag = t.color ? t.color : C_MUTED;
  }
  if (t.tag[0]) drawTag(t.tag, x + 6, y + 4, tag);

  const int tx = x + 4, tw = w - 8, ty = y + kTagH + 2, th = h - kTagH - 5;
  Lines L;
  layoutLines(t.text[0] ? t.text : "-", tw, L);
  if (L.n == 1) {
    drawFit(L.line[0], tx, ty, tw, th, text, kOneLine, 3);
  } else {
    const GFXfont* const f[] = {commonFont2(L.line, tw, th / 2)};
    drawFit(L.line[0], tx, ty, tw, th / 2, text, f, 1);
    drawFit(L.line[1], tx, ty + th / 2, tw, th - th / 2, text, f, 1);
  }
}

}  // namespace

// ------------------------------------------------------------------ geometry

void tileRect(int W, int H, int i, int& x, int& y, int& w, int& h) {
  const int cols = W > H ? 3 : 2, rows = 6 / cols;
  const int top = kNameTop + nameHeight(W, H) + 4;
  w = (W - 2 * kMargin - (cols - 1) * kGap) / cols;
  h = (H - top - kMargin - (rows - 1) * kGap) / rows;
  x = kMargin + (i % cols) * (w + kGap);
  y = top + (i / cols) * (h + kGap);
}

int tileAt(int W, int H, int px, int py) {
  for (int i = 0; i < 6; i++) {
    int x, y, w, h;
    tileRect(W, H, i, x, y, w, h);
    // half a gap of tolerance around each tile
    if (px >= x - kGap / 2 && px < x + w + kGap / 2 && py >= y - kGap / 2 && py < y + h + kGap / 2) return i;
  }
  return -1;
}

bool statusBarAt(int W, int H, int x, int y) {
  (void)W;
  (void)H;
  (void)x;
  return y < kNameTop - 2;
}

// ------------------------------------------------------------------ screens

void renderCalibration(Adafruit_GFX& gfx, int step, int px, int py, const char* hint) {
  g = &gfx;
  gfx.setTextWrap(false);
  const int W = gfx.width(), H = gfx.height();
  gfx.fillScreen(C_BG);
  char title[40];
  snprintf(title, sizeof title, "Calibrate touch (%d/3)", step + 1);
  drawFit(title, 0, H / 2 - 34, W, 26, C_TEXT, kMid + 1, 2);
  drawFit(hint, 0, H / 2, W, 22, C_MUTED, kSmall, 1);
  if (px >= 0) drawCross(px, py, C_ACCENT);
}

void renderScreen(Adafruit_GFX& gfx, const ScreenState& s) {
  g = &gfx;
  gfx.setTextWrap(false);
  const int W = gfx.width(), H = gfx.height();
  gfx.fillScreen(C_BG);

  if (!s.usbConnected || !s.online) {
    drawStatusLine("AMPERO II", s.usbConnected);
    drawFit("Ampero not connected", 0, H / 2 - 40, W, 40, C_TEXT, kMid, 3);
    drawFit("Connect the USB cable to the Ampero", 0, H / 2 + 8, W, 24, C_MUTED, kSmall, 1);
    return;
  }
  if (!s.hasPreset) {
    drawStatusLine("AMPERO II", true);
    drawFit("Reading preset ...", 0, H / 2 - 25, W, 50, C_TEXT, kMid, 3);
    return;
  }

  char head[32];
  // like the device display: 3 patches per bank, A00-1 .. A99-3
  snprintf(head, sizeof head, "A%02d-%d", s.presetIndex / 3, s.presetIndex % 3 + 1);
  drawStatusLine(head, true);
  const int nameH = nameHeight(W, H) - kSceneLineH;
  drawFit(s.presetName, kMargin, kNameTop, W - 2 * kMargin, nameH, C_TEXT, kBig, 4);
  if (s.sceneName && s.sceneName[0])
    drawFit(s.sceneName, kMargin, kNameTop + nameH, W - 2 * kMargin, kSceneLineH, C_ACCENT, kMid + 1, 2);

  for (int i = 0; i < 6; i++) {
    int x, y, w, h;
    tileRect(W, H, i, x, y, w, h);
    drawTile(s.tiles[i], x, y, w, h);
  }
}

// ------------------------------------------------------------------ menu

namespace {

void gridCell(int W, int H, const MenuView& v, int i, int& x, int& y, int& w, int& h) {
  const int cols = v.gridCols;
  const int rows = (v.nRows + cols - 1) / cols;
  w = (W - 2 * kMargin - (cols - 1) * kGap) / cols;
  h = (H - kMenuTop - kMargin - (rows - 1) * kGap) / rows;
  if (h > 56) h = 56;
  x = kMargin + (i % cols) * (w + kGap);
  y = kMenuTop + (i / cols) * (h + kGap);
}

void listRow(int W, int i, int& x, int& y, int& w, int& h) {
  x = kMargin;
  w = W - 2 * kMargin;
  y = kMenuTop + i * (kMenuRowH + kMenuGap);
  h = kMenuRowH;
}

int valueWidth(int W) { return W > 240 ? 112 : 84; }
constexpr int kArrowW = 32;
constexpr int kToggleW = 76;

}  // namespace

void renderMenu(Adafruit_GFX& gfx, const MenuView& v) {
  g = &gfx;
  gfx.setTextWrap(false);
  const int W = gfx.width(), H = gfx.height();
  gfx.fillScreen(C_BG);

  // header: back button + title
  gfx.fillRoundRect(kMargin, 3, 44, 24, 5, C_TILE);
  gfx.fillTriangle(kMargin + 16, 15, kMargin + 27, 8, kMargin + 27, 22, C_TEXT);
  drawFit(v.title, 56, 3, W - 62, 24, C_ACCENT, kRow, 2, -1);

  for (int i = 0; i < v.nRows; i++) {
    const MenuRow& r = v.rows[i];
    int x, y, w, h;
    if (v.gridCols) {
      gridCell(W, H, v, i, x, y, w, h);
      const uint16_t c = r.color ? r.color : C_TILE_BORDER;
      gfx.fillRoundRect(x, y, w, h, 7, C_TILE);
      gfx.drawRoundRect(x, y, w, h, 7, c);
      if (r.value[0]) {
        drawFit(r.label, x + 4, y + 3, w - 8, h / 2 - 2, C_TEXT, kRow, 2);
        drawFit(r.value, x + 4, y + h / 2, w - 8, h / 2 - 3, r.color ? r.color : C_MUTED, kRow + 1, 1);
      } else {
        drawFit(r.label, x + 4, y + 3, w - 8, h - 6, C_TEXT, kRow, 2);
      }
      continue;
    }
    listRow(W, i, x, y, w, h);
    gfx.fillRoundRect(x, y, w, h, 6, C_ROW);
    const int vw = valueWidth(W);
    // label width: value rows leave room for arrows + value, toggles for the pill, links for the value
    const int labelW = r.kind == ROW_VALUE ? w - vw - 2 * kArrowW - 14
                       : r.kind == ROW_TOGGLE ? w - kToggleW - 18
                                              : w - vw - kArrowW - 10;
    if (r.kind == ROW_INFO && !r.value[0]) {
      drawFit(r.label, x + 8, y, w - 16, h, C_MUTED, kRow + 1, 1, -1);
      continue;
    }
    drawFit(r.label, x + 8, y, labelW, h, C_TEXT, kRow + 1, 1, -1);
    const int rx = x + w - 2 * kArrowW - vw - 4;   // value area (with arrows)
    switch (r.kind) {
      case ROW_VALUE:
        gfx.fillRoundRect(rx, y + 2, kArrowW, h - 4, 5, C_TILE);
        gfx.fillTriangle(rx + 10, y + h / 2, rx + 21, y + 8, rx + 21, y + h - 8, C_TEXT);
        gfx.fillRoundRect(x + w - kArrowW - 2, y + 2, kArrowW, h - 4, 5, C_TILE);
        gfx.fillTriangle(x + w - 12, y + h / 2, x + w - 23, y + 8, x + w - 23, y + h - 8, C_TEXT);
        drawFit(r.value, rx + kArrowW + 2, y, vw, h, r.color ? r.color : C_ACCENT, kRow, 2);
        break;
      case ROW_TOGGLE:
        gfx.fillRoundRect(x + w - kToggleW - 4, y + 4, kToggleW, h - 8, 8, r.color ? r.color : C_TILE);
        drawFit(r.value, x + w - kToggleW - 4, y + 4, kToggleW, h - 8, r.color ? C_DARK_TEXT : C_TEXT, kRow, 2);
        break;
      case ROW_LINK:
        drawFit(r.value, rx, y, vw + kArrowW, h, C_MUTED, kRow + 1, 1, 1);
        gfx.fillTriangle(x + w - 10, y + h / 2, x + w - 20, y + 8, x + w - 20, y + h - 8, C_ACCENT);
        break;
      default:
        drawFit(r.value, rx, y, vw + 2 * kArrowW, h, r.color ? r.color : C_ACCENT, kRow, 2, 1);
        break;
    }
  }
}

MenuHit menuHitTest(int W, int H, const MenuView& v, int px, int py) {
  if (py < kMenuTop - 2) return {px < 80 ? -1 : -2, 0};
  for (int i = 0; i < v.nRows; i++) {
    int x, y, w, h;
    if (v.gridCols) gridCell(W, H, v, i, x, y, w, h);
    else listRow(W, i, x, y, w, h);
    if (py < y - kMenuGap / 2 - 1 || py >= y + h + kMenuGap / 2 + 1 || px < x || px >= x + w) continue;
    if (v.gridCols || v.rows[i].kind != ROW_VALUE) return {i, 0};
    const int rx = x + w - 2 * kArrowW - valueWidth(W) - 4;
    if (px >= x + w - kArrowW - 6) return {i, +1};
    if (px >= rx - 4 && px < rx + kArrowW + 4) return {i, -1};
    return {i, 0};
  }
  return {-2, 0};
}
