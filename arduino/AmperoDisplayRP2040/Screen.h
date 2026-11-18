#pragma once
#include <Adafruit_GFX.h>

/** Everything the screen shows. */
struct ScreenState {
  bool usbConnected = false;
  bool online = false;      // Ampero answered / protocol running
  bool hasPreset = false;
  int presetIndex = -1;
  const char* presetName = "";
  const char* sceneNames[3] = {"", "", ""};
  int activeScene = -1;
};

/**
 * Draws the whole screen (layout scales with width()/height(); landscape recommended).
 * Works with any Adafruit_GFX target, e.g. the strip canvas (called once per strip).
 */
void renderScreen(Adafruit_GFX& gfx, const ScreenState& s);
