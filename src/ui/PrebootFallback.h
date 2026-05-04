

#pragma once

#include <TFT_eSPI.h>

namespace PrebootFallback {

// Tiny pre-LVGL / LVGL-failed fallback only.
// Do not use for normal runtime UI.
void showInit(TFT_eSPI& tft, const char* line1 = "SPECTRE", const char* line2 = "INITIALIZING...");
void showFatal(TFT_eSPI& tft, const char* line1 = "SPECTRE", const char* line2 = "DISPLAY FAILURE");

} // namespace PrebootFallback


