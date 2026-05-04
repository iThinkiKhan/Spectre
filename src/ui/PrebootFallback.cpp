

#include "PrebootFallback.h"

namespace PrebootFallback {

static void drawCenteredPair(TFT_eSPI& tft, uint16_t fg, uint16_t bg,
                             const char* line1, const char* line2) {
    tft.fillScreen(bg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, bg);

    // Keep this intentionally simple and fast.
    tft.drawString(line1 ? line1 : "SPECTRE", 160, 70, 4);
    if (line2 && line2[0]) {
        tft.drawString(line2, 160, 100, 2);
    }
}

void showInit(TFT_eSPI& tft, const char* line1, const char* line2) {
    drawCenteredPair(tft, TFT_WHITE, TFT_BLACK, line1, line2);
}

void showFatal(TFT_eSPI& tft, const char* line1, const char* line2) {
    drawCenteredPair(tft, TFT_RED, TFT_BLACK, line1, line2);
}

} // namespace PrebootFallback


