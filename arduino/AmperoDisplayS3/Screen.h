#pragma once
#include <Adafruit_GFX.h>

/** One of the six tiles (touch footswitches). */
struct TileView {
  char tag[24] = "";      // small label top left, e.g. "RVB", "SCENE 1", "FS2 DLY"
  char text[64] = "";     // main text, lines separated by '\n' (at most two are shown)
  uint16_t color = 0;     // tile colour (effect category / scene accent)
  int8_t state = -1;      // 1 on (filled), 0 off (outlined), -1 no state
};

/** Everything the main screen shows. */
struct ScreenState {
  bool usbConnected = false;
  bool online = false;      // Ampero answered / protocol running
  bool hasPreset = false;
  int presetIndex = -1;
  const char* presetName = "";
  const char* sceneName = "";   // active scene, shown below the preset name
  TileView tiles[6];
};

/**
 * Draws the whole screen (layout scales with width()/height(): 320x240 .. 480x320, landscape or portrait).
 * Works with any Adafruit_GFX target, e.g. the strip canvas (called once per strip).
 */
void renderScreen(Adafruit_GFX& gfx, const ScreenState& s);

/** Rectangle of tile i (0..5: landscape 3x2, portrait 2x3, row by row). */
void tileRect(int W, int H, int i, int& x, int& y, int& w, int& h);
/** Tile under the screen point, or -1. */
int tileAt(int W, int H, int x, int y);
/** Status bar at the top (opens the menu). */
bool statusBarAt(int W, int H, int x, int y);

// ------------------------------------------------------------------ menu

enum MenuRowKind : uint8_t {
  ROW_INFO = 0,   // text only
  ROW_VALUE,      // value with < > arrows (tap on the row = >)
  ROW_LINK,       // opens a sub page (">")
  ROW_TOGGLE,     // tap toggles the value
};

struct MenuRow {
  char label[28] = "";
  char value[40] = "";
  uint8_t kind = ROW_INFO;
  uint16_t color = 0;   // grid cells: accent colour (0 = default)
};

struct MenuView {
  char title[32] = "";
  int nRows = 0;
  int gridCols = 0;     // 0 = list, otherwise cells in a grid
  MenuRow rows[12];
};

void renderMenu(Adafruit_GFX& gfx, const MenuView& v);

/** Hit test: row -1 = back button, -2 = nothing; zone -1 = left arrow, +1 = right arrow, 0 = row. */
struct MenuHit {
  int row;
  int zone;
};
MenuHit menuHitTest(int W, int H, const MenuView& v, int x, int y);
