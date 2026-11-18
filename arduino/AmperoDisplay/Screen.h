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

/** Draws the whole screen into the canvas (any rotation; landscape recommended). */
void renderScreen(GFXcanvas16& canvas, const ScreenState& s);
