
#include "Mascot.h"


// TODO(LVGL_MIGRATION):
// Mascot rendering is still tied to TFT/sprite-era assumptions.
// Goal: move runtime mascot rendering to LVGL-owned objects/canvas/images.
// Keep no direct runtime sprite/TFT rendering once migration is complete.


// RENDER OWNERSHIP
// ----------------
// Runtime UI is LVGL-only.
// Direct TFT access is allowed only:
//   1) inside the LVGL flush driver
//   2) in explicit boot/fatal fallback code
// Do not add new runtime TFT or sprite rendering paths here.


// The LVGL theme uses 24-bit colors, but TFT_eSprite drawing APIs want RGB565.
// Remap the shared palette locally so the recovered sprite code compiles cleanly.
#undef CLR_BLACK
#undef CLR_WHITE
#undef CLR_YELLOW
#undef CLR_CYAN
#undef CLR_RED
#undef CLR_GREEN
#undef CLR_HOTPINK
#undef CLR_DIM
#undef CLR_DIMYELLOW
#undef CLR_DIMCYAN
#undef CLR_CHROME
#undef CLR_NAVY
#undef CLR_ORANGE
#define CLR_BLACK       0x0000
#define CLR_WHITE       0xFFFF
#define CLR_YELLOW      0xFCC0
#define CLR_CYAN        0x07FF
#define CLR_RED         0xF800
#define CLR_GREEN       0x07E0
#define CLR_HOTPINK     0xF81F
#define CLR_DIM         0x7BEF
#define CLR_DIMYELLOW   0xA500
#define CLR_DIMCYAN     0x0451
#define CLR_CHROME      0x2104
#define CLR_NAVY        0x000F
#define CLR_ORANGE      0xFD20

// Transparent color key — used as sprite background
// Must not appear in any sprite art
#define TRANSPARENT 0x0821

bool Mascot::begin(TFT_eSPI* tft) {
    _tft = tft;

    _spr = new TFT_eSprite(tft);
    if (!_spr->createSprite(SPR_FULL_W, SPR_FULL_H)) {
        Serial.println("[MASCOT] Full sprite alloc failed");
        return false;
    }
    _spr->setColorDepth(16);
    _spr->setSwapBytes(false);

    _sprSm = new TFT_eSprite(tft);
    _sprSm->setColorDepth(16);
    if (!_sprSm->createSprite(SPR_SMALL_W, SPR_SMALL_H)) {
        Serial.println("[MASCOT] Small sprite alloc failed");
        return false;
    }

    Serial.println("[MASCOT] Sprites allocated in PSRAM");
    return true;
}

void Mascot::end() {
    if (_spr)   { _spr->deleteSprite();   delete _spr;   _spr = nullptr; }
    if (_sprSm) { _sprSm->deleteSprite(); delete _sprSm; _sprSm = nullptr; }
}

// ─── Public draw ─────────────────────────────────────────────

void Mascot::draw(int screenX, int screenY, MascotState state, int frame) {
    if (!_spr) return;
    _spr->fillSprite(TFT_TRANSPARENT);

    switch(state) {
        case MASCOT_BOOT_ATTENTION: _drawBoot(frame);       break;
        case MASCOT_STANDBY:        _drawStandby(frame);    break;
        case MASCOT_RECON_WALK:     _drawReconWalk(frame);  break;
        case MASCOT_WIFI_RECON:     _drawWifiRecon(frame);  break;
        case MASCOT_LORA_RECON:     _drawLoraRecon(frame);  break;
        case MASCOT_PWNY:           _drawPwny(frame);       break;
        case MASCOT_BAD_USB:        _drawBadUSB(frame);     break;
        case MASCOT_HOMELAB_SYNC:   _drawHomelab(frame);    break;
        case MASCOT_ALERT:          _drawAlert(frame);      break;
        case MASCOT_TRANSMIT:       _drawTransmit(frame);   break;
        case MASCOT_LOW_BATTERY:    _drawLowBattery(frame); break;
        case MASCOT_ERROR:          _drawError(frame);      break;
        case MASCOT_PREFLIGHT:      _drawPreflight(frame);  break;
    }

    _spr->pushSprite(screenX, screenY, TFT_TRANSPARENT);
}

void Mascot::drawSmall(int screenX, int screenY, MascotState state, int frame) {
    if (!_sprSm) return;
    _sprSm->fillSprite(CLR_BLACK);

    int cx = SPR_SMALL_W / 2;
    int cy = SPR_SMALL_H / 2 - 6;

    uint16_t bodyColor = CLR_WHITE;
    if (state == MASCOT_PWNY)        bodyColor = CLR_HOTPINK;
    if (state == MASCOT_ERROR)       bodyColor = CLR_RED;
    if (state == MASCOT_LOW_BATTERY) bodyColor = (frame % 6 < 1) ? CLR_DIM : CLR_WHITE;

    _smallGhostBody(cx, cy, bodyColor);

    uint16_t eyeColor = (state == MASCOT_PWNY) ? CLR_RED : CLR_BLACK;
    _smallEyes(cx, cy, eyeColor);

    // Mode indicator dot
    if (state == MASCOT_RECON_WALK ||
        state == MASCOT_WIFI_RECON ||
        state == MASCOT_LORA_RECON) {
        uint16_t dotColor = (frame % 4 < 2) ? CLR_YELLOW : CLR_DIMYELLOW;
        _sprSm->fillCircle(cx, cy - 20, 3, dotColor);
    }
    if (state == MASCOT_PWNY) {
        _sprSm->fillTriangle(cx - 6, cy - 14, cx - 9, cy - 20, cx - 3, cy - 14, CLR_RED);
        _sprSm->fillTriangle(cx + 3, cy - 14, cx + 6, cy - 20, cx + 9, cy - 14, CLR_RED);
    }
    if (state == MASCOT_STANDBY) {
        // Half closed eyes
        _sprSm->fillRect(cx - 7, cy - 6, 5, 2, bodyColor);
        _sprSm->fillRect(cx + 2, cy - 6, 5, 2, bodyColor);
    }
    if (state == MASCOT_HOMELAB_SYNC) {
        // Tiny glasses
        _sprSm->drawRect(cx - 8, cy - 5, 6, 4, CLR_DIM);
        _sprSm->drawRect(cx + 2, cy - 5, 6, 4, CLR_DIM);
        _sprSm->drawLine(cx - 2, cy - 3, cx + 2, cy - 3, CLR_DIM);
    }

    _sprSm->pushSprite(screenX, screenY);
}

void Mascot::clearFull(int screenX, int screenY) {
    _tft->fillRect(screenX, screenY, SPR_FULL_W, SPR_FULL_H, CLR_BLACK);
}

void Mascot::clearSmall(int screenX, int screenY) {
    _tft->fillRect(screenX, screenY, SPR_SMALL_W, SPR_SMALL_H, CLR_BLACK);
}

// ─── Shared body parts ───────────────────────────────────────
// All draw into _spr. cx/cy = center of ghost body.

void Mascot::_ghostBody(int cx, int cy, uint16_t color) {
    // Head/upper body ellipse
    _spr->fillEllipse(cx, cy, 22, 24, color);
    // Lower body rectangle
    _spr->fillRect(cx - 22, cy, 44, 18, color);
    // Three bottom bumps
    _spr->fillCircle(cx - 14, cy + 18, 9, color);
    _spr->fillCircle(cx,      cy + 18, 9, color);
    _spr->fillCircle(cx + 14, cy + 18, 9, color);
    // Cut between bumps — black notches
    _spr->fillRect(cx - 22, cy + 22, 44, 8, CLR_BLACK);
    _spr->fillCircle(cx - 7, cy + 24, 5, CLR_BLACK);
    _spr->fillCircle(cx + 7, cy + 24, 5, CLR_BLACK);
    // Subtle outline for definition
    if (color == CLR_WHITE) {
        _spr->drawEllipse(cx, cy, 22, 24, CLR_DIM);
    }
}

void Mascot::_eyes(int cx, int cy, bool wide, bool angry,
                   bool halfClosed, uint16_t color) {
    int r = wide ? 4 : 3;
    _spr->fillCircle(cx - 8, cy - 4, r, color);
    _spr->fillCircle(cx + 8, cy - 4, r, color);
    if (halfClosed) {
        // Draw white rect over top half of eyes
        _spr->fillRect(cx - 13, cy - 8, 11, 4, CLR_WHITE);
        _spr->fillRect(cx + 3,  cy - 8, 11, 4, CLR_WHITE);
    }
    if (angry) {
        _spr->drawLine(cx - 13, cy - 9, cx - 4, cy - 7, CLR_BLACK);
        _spr->drawLine(cx + 4,  cy - 7, cx + 13, cy - 9, CLR_BLACK);
    }
}

void Mascot::_mouth(int cx, int cy, int type) {
    switch(type) {
        case 0: // neutral
            _spr->drawLine(cx - 5, cy + 8, cx + 5, cy + 8, CLR_DIM);
            break;
        case 1: // grin
            _spr->fillRect(cx - 7, cy + 6, 14, 6, CLR_BLACK);
            _spr->fillRect(cx - 5, cy + 7, 4, 4, CLR_WHITE);
            _spr->fillRect(cx - 0, cy + 7, 4, 4, CLR_WHITE);
            _spr->fillRect(cx + 5, cy - 1 + 8, 3, 3, CLR_WHITE);
            break;
        case 2: // frown
            _spr->drawLine(cx - 5, cy + 10, cx,     cy + 7, CLR_DIM);
            _spr->drawLine(cx,     cy + 7,  cx + 5, cy + 10, CLR_DIM);
            break;
        case 3: // open/surprised
            _spr->fillEllipse(cx, cy + 8, 5, 4, CLR_DIM);
            break;
    }
}

void Mascot::_helmet(int cx, int cy) {
    // Olive drab helmet — too big, slightly tilted
    uint16_t olive = 0x5DC0;   // brighter olive green
    uint16_t dark  = 0x3A80;   // darker shadow
    _spr->fillEllipse(cx + 2, cy - 26, 26, 10, olive);
    _spr->fillRect(cx - 24, cy - 28, 50, 8, olive);
    _spr->fillEllipse(cx + 2, cy - 32, 20, 8, dark);
    // Brim highlight
    _spr->drawLine(cx - 24, cy - 22, cx + 26, cy - 22, 0x6B60);
    // Loose chin straps
    _spr->drawLine(cx - 20, cy - 24, cx - 24, cy - 8, CLR_DIM);
    _spr->drawLine(cx - 24, cy - 8,  cx - 18, cy - 2, CLR_DIM);
    _spr->drawLine(cx + 16, cy - 24, cx + 20, cy - 10, CLR_DIM);
}

void Mascot::_headphones(int cx, int cy, int frame) {
    // Headband
    _spr->drawArc(cx, cy - 20, 22, 18, 195, 345, CLR_CYAN, CLR_BLACK);
    // Ear cups
    _spr->fillCircle(cx - 21, cy - 10, 7, CLR_CYAN);
    _spr->fillCircle(cx + 21, cy - 10, 7, CLR_CYAN);
    _spr->fillCircle(cx - 21, cy - 10, 4, CLR_DIMCYAN);
    _spr->fillCircle(cx + 21, cy - 10, 4, CLR_DIMCYAN);
    // Pulse ring
    if (frame % 6 < 3) {
        _spr->drawCircle(cx - 21, cy - 10, 9,  CLR_CYAN);
        _spr->drawCircle(cx + 21, cy - 10, 9,  CLR_CYAN);
    }
    if (frame % 6 < 1) {
        _spr->drawCircle(cx - 21, cy - 10, 11, CLR_DIMCYAN);
        _spr->drawCircle(cx + 21, cy - 10, 11, CLR_DIMCYAN);
    }
}

void Mascot::_satDish(int cx, int cy, int frame) {
    int dx = cx + 32;
    int dy = cy + 8;
    // Stand
    _spr->drawLine(dx, dy, dx, dy - 16, CLR_DIM);
    // Dish
    _spr->fillEllipse(dx, dy - 18, 10, 5, CLR_DIM);
    _spr->drawLine(dx, dy - 23, dx - 8, dy - 30, CLR_DIM);
    // Signal rings
    int phase = frame % 8;
    if (phase > 0) _spr->drawCircle(dx - 10, dy - 32, 4,  CLR_YELLOW);
    if (phase > 2) _spr->drawCircle(dx - 14, dy - 36, 8,  CLR_DIMYELLOW);
    if (phase > 4) _spr->drawCircle(dx - 18, dy - 40, 12, 0x4208);
}

void Mascot::_devilHorns(int cx, int cy) {
    _spr->fillTriangle(cx - 10, cy - 26, cx - 18, cy - 42, cx - 4,  cy - 26, CLR_RED);
    _spr->fillTriangle(cx + 4,  cy - 26, cx + 12, cy - 44, cx + 18, cy - 26, CLR_RED);
    // Horn highlight
    _spr->drawLine(cx - 16, cy - 40, cx - 12, cy - 28, 0xF810);
    _spr->drawLine(cx + 10, cy - 42, cx + 14, cy - 28, 0xF810);
}

void Mascot::_claws(int cx, int cy, int frame) {
    int flex = (frame % 6 < 3) ? 2 : 0;
    // Left arm
    _spr->fillEllipse(cx - 26, cy + 4, 7, 5, CLR_WHITE);
    _spr->drawLine(cx - 33, cy + 2, cx - 38, cy - 1 - flex, CLR_YELLOW);
    _spr->drawLine(cx - 33, cy + 4, cx - 38, cy + 5,         CLR_YELLOW);
    _spr->drawLine(cx - 33, cy + 6, cx - 37, cy + 10 + flex, CLR_YELLOW);
    // Right arm
    _spr->fillEllipse(cx + 26, cy + 4, 7, 5, CLR_WHITE);
    _spr->drawLine(cx + 33, cy + 2, cx + 38, cy - 1 - flex, CLR_YELLOW);
    _spr->drawLine(cx + 33, cy + 4, cx + 38, cy + 5,         CLR_YELLOW);
    _spr->drawLine(cx + 33, cy + 6, cx + 37, cy + 10 + flex, CLR_YELLOW);
}

void Mascot::_glasses(int cx, int cy) {
    _spr->drawRect(cx - 14, cy - 7, 12, 8, CLR_DIM);
    _spr->drawRect(cx + 2,  cy - 7, 12, 8, CLR_DIM);
    _spr->drawLine(cx - 2,  cy - 3, cx + 2,  cy - 3, CLR_DIM);
    _spr->drawLine(cx - 26, cy - 3, cx - 14, cy - 3, CLR_DIM);
    _spr->drawLine(cx + 14, cy - 3, cx + 26, cy - 3, CLR_DIM);
}

void Mascot::_trenchcoat(int cx, int cy, int frame) {
    int sway = (frame % 8 < 4) ? 3 : -3;
    _spr->fillTriangle(cx - 22, cy + 12,
                       cx - 30, cy + 38,
                       cx - 12, cy + 38, CLR_CHROME);
    _spr->fillTriangle(cx + 22, cy + 12,
                       cx + 12, cy + 38,
                       cx + 30 + sway, cy + 38, CLR_CHROME);
    // Lapel lines
    _spr->drawLine(cx - 4,  cy + 12, cx - 10, cy + 24, CLR_DIM);
    _spr->drawLine(cx + 4,  cy + 12, cx + 10, cy + 24, CLR_DIM);
    // Button
    _spr->fillCircle(cx, cy + 20, 2, CLR_DIM);
}

void Mascot::_wifiRings(int cx, int cy, int frame) {
    // Scanner device
    _spr->fillRect(cx + 18, cy - 14, 10, 14, CLR_CHROME);
    _spr->drawRect(cx + 18, cy - 14, 10, 14, CLR_CYAN);
    _spr->fillRect(cx + 20, cy - 12, 6,  4,  CLR_NAVY);
    // Arm holding scanner
    _spr->fillEllipse(cx + 14, cy + 2, 7, 4, CLR_WHITE);
    // Rings
    int phase = (frame % 8) + 1;
    if (phase > 1) _spr->drawCircle(cx + 38, cy - 8, 5,  CLR_CYAN);
    if (phase > 3) _spr->drawCircle(cx + 38, cy - 8, 10, CLR_DIMCYAN);
    if (phase > 5) _spr->drawCircle(cx + 38, cy - 8, 15, 0x0229);
}

void Mascot::_soundWaves(int cx, int cy, int frame) {
    int phase = frame % 6;
    uint16_t c1 = phase > 0 ? CLR_YELLOW    : CLR_DIMYELLOW;
    uint16_t c2 = phase > 2 ? CLR_YELLOW    : CLR_DIMYELLOW;
    uint16_t c3 = phase > 4 ? CLR_DIMYELLOW : CLR_DIM;
    _spr->drawArc(cx + 30, cy, 8,  6,  310, 50, c1, CLR_BLACK);
    _spr->drawArc(cx + 30, cy, 14, 12, 310, 50, c2, CLR_BLACK);
    _spr->drawArc(cx + 30, cy, 20, 18, 310, 50, c3, CLR_BLACK);
}

void Mascot::_sparks(int cx, int cy, int frame) {
    if (frame % 2 == 0) {
        _spr->drawLine(cx + 22, cy,     cx + 28, cy - 7,  CLR_YELLOW);
        _spr->drawLine(cx + 28, cy - 7, cx + 24, cy - 12, CLR_ORANGE);
        _spr->fillCircle(cx + 26, cy - 4, 2, CLR_WHITE);
    } else {
        _spr->drawLine(cx + 20, cy - 2, cx + 27, cy - 9,  CLR_YELLOW);
        _spr->drawLine(cx + 18, cy - 6, cx + 25, cy - 13, CLR_ORANGE);
        _spr->fillCircle(cx + 22, cy - 8, 2, CLR_WHITE);
    }
}

void Mascot::_dataTrail(int cx, int cy, int frame) {
    // Dots trailing behind ghost (to the left)
    int phase = frame % 12;
    uint16_t colors[] = {CLR_YELLOW, CLR_DIMYELLOW, CLR_DIM, 0x2104};
    for (int i = 0; i < 4; i++) {
        int dotX = cx - 30 - (i * 9) - (phase < 6 ? 1 : 0);
        int dotY = cy + 14;
        int r    = (i < 2) ? 2 : 1;
        _spr->fillCircle(dotX, dotY, r, colors[i]);
    }
}

void Mascot::_smallGhostBody(int cx, int cy, uint16_t color) {
    _sprSm->fillEllipse(cx, cy, 18, 20, color);
    _sprSm->fillRect(cx - 18, cy, 36, 14, color);
    _sprSm->fillCircle(cx - 11, cy + 14, 8, color);
    _sprSm->fillCircle(cx,      cy + 14, 8, color);
    _sprSm->fillCircle(cx + 11, cy + 14, 8, color);
    _sprSm->fillRect(cx - 18, cy + 18, 36, 6, CLR_BLACK);
    _sprSm->fillCircle(cx - 6, cy + 19, 4, CLR_BLACK);
    _sprSm->fillCircle(cx + 6, cy + 19, 4, CLR_BLACK);
}

void Mascot::_smallEyes(int cx, int cy, uint16_t color) {
    _sprSm->fillCircle(cx - 6, cy - 4, 3, color);
    _sprSm->fillCircle(cx + 6, cy - 4, 3, color);
}

// ─── Full sprite state draws ──────────────────────────────────
// All draw at center cx=45, cy=58 within the 90x110 sprite

void Mascot::_drawBoot(int frame) {
    int cx = 45, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    _helmet(cx, cy);
    _eyes(cx, cy, false, false, false, CLR_BLACK);
    _mouth(cx, cy, 0);

    // Salute animation
    if (frame < 10) {
        int progress = frame * 2;
        int armX = cx + 14 + progress / 3;
        int armY = cy + 2  - progress;
        _spr->fillEllipse(armX, armY, 7, 5, CLR_WHITE);
    } else {
        // Held salute
        _spr->fillEllipse(cx + 22, cy - 16, 6, 5, CLR_WHITE);
        _spr->drawLine(cx + 14, cy + 2, cx + 20, cy - 14, CLR_WHITE);
        // Held expression — slight smile
        _spr->drawLine(cx - 4, cy + 8, cx,     cy + 7, CLR_DIM);
        _spr->drawLine(cx,     cy + 7, cx + 4, cy + 8, CLR_DIM);
    }
}

void Mascot::_drawStandby(int frame) {
    int cx = 45, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    _headphones(cx, cy, frame);
    _eyes(cx, cy, false, false, true, CLR_BLACK);
    _mouth(cx, cy, 0);
    _satDish(cx, cy, frame);

    // Occasional yawn
    if (frame % 100 > 90) {
        _mouth(cx, cy, 3);
        // Tired eyes fully closed
        _spr->fillRect(cx - 13, cy - 8, 11, 6, CLR_WHITE);
        _spr->fillRect(cx + 2,  cy - 8, 11, 6, CLR_WHITE);
    }
}

void Mascot::_drawReconWalk(int frame) {
    int cx = 45, cy = 58;
    int bob = (frame % 8 < 4) ? -1 : 1;
    _trenchcoat(cx, cy + bob, frame);
    _ghostBody(cx, cy + bob, CLR_WHITE);
    // Scanning eyes — shift left/right
    int eyeShift = (frame % 10 < 5) ? -3 : 3;
    _eyes(cx + eyeShift, cy + bob, true, false, false, CLR_BLACK);
    _mouth(cx, cy + bob, 0);
    _dataTrail(cx, cy + bob, frame);
}

void Mascot::_drawWifiRecon(int frame) {
    int cx = 40, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    _eyes(cx, cy, false, false, false, CLR_BLACK);
    // Focused brow
    _spr->drawLine(cx - 12, cy - 9, cx - 5, cy - 7, CLR_BLACK);
    _spr->drawLine(cx + 5,  cy - 7, cx + 12, cy - 9, CLR_BLACK);
    _mouth(cx, cy, 0);
    _wifiRings(cx, cy, frame);
}

void Mascot::_drawLoraRecon(int frame) {
    int cx = 42, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    // Big ear on right
    _spr->fillEllipse(cx + 26, cy - 2, 9, 14, CLR_WHITE);
    _spr->fillEllipse(cx + 26, cy - 2, 5, 9,  CLR_DIM);
    _spr->fillEllipse(cx + 26, cy - 2, 2, 4,  CLR_BLACK);
    _eyes(cx, cy, false, false, false, CLR_BLACK);
    _mouth(cx, cy, 0);
    // Signal waves coming in
    int phase = frame % 8;
    if (phase > 0) _spr->drawLine(cx + 40, cy - 2, cx + 37, cy - 2, CLR_YELLOW);
    if (phase > 2) { _spr->drawArc(cx + 44, cy - 2, 6,  4, 160, 200, CLR_YELLOW,    CLR_BLACK); }
    if (phase > 4) { _spr->drawArc(cx + 44, cy - 2, 12, 9, 160, 200, CLR_DIMYELLOW, CLR_BLACK); }
    if (phase > 6) { _spr->drawArc(cx + 44, cy - 2, 18, 14,160, 200, CLR_DIM,       CLR_BLACK); }
}

void Mascot::_drawPwny(int frame) {
    int cx = 45, cy = 60;
    _ghostBody(cx, cy, CLR_WHITE);
    _devilHorns(cx, cy);
    _eyes(cx, cy, true, true, false, CLR_RED);
    _mouth(cx, cy, 1);
    _claws(cx, cy, frame);

    // Evil aura pulse
    if (frame % 4 < 2) {
        _spr->drawCircle(cx, cy, 30, 0xA000);
        _spr->drawCircle(cx, cy, 32, 0x6000);
    }
    // Evil laugh shake
    if (frame % 80 > 72) {
        int shake = (frame % 2) ? 1 : -1;
        _spr->fillCircle(cx + shake, cy - 44, 3, CLR_RED);
    }
}

void Mascot::_drawBadUSB(int frame) {
    int cx = 44, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    // Looking sideways — suspicious glance right
    _spr->fillCircle(cx - 6, cy - 4, 3, CLR_BLACK);
    _spr->fillCircle(cx + 10, cy - 4, 3, CLR_BLACK);
    _mouth(cx, cy, 0);

    // USB plug in hand
    _spr->fillRect(cx + 14, cy + 2,  12, 7, CLR_DIM);
    _spr->fillRect(cx + 20, cy + 3,  6,  5, CLR_CHROME);
    _spr->drawRect(cx + 14, cy + 2,  12, 7, CLR_YELLOW);
    _spr->fillRect(cx + 22, cy + 4,  2,  3, CLR_BLACK);
    _spr->fillRect(cx + 25, cy + 4,  2,  3, CLR_BLACK);

    // Wall socket
    _spr->fillRect(cx + 34, cy - 10, 8, 26, CLR_CHROME);
    _spr->drawRect(cx + 34, cy - 10, 8, 26, CLR_DIM);
    _spr->fillRect(cx + 36, cy - 2,  2,  5, CLR_BLACK);
    _spr->fillRect(cx + 39, cy - 2,  2,  5, CLR_BLACK);

    _sparks(cx, cy, frame);
}

void Mascot::_drawHomelab(int frame) {
    int cx = 44, cy = 62;
    _ghostBody(cx, cy, CLR_WHITE);
    _glasses(cx, cy - 4);
    // Eyes looking down
    _spr->fillCircle(cx - 8, cy - 2, 3, CLR_BLACK);
    _spr->fillCircle(cx + 8, cy - 2, 3, CLR_BLACK);
    _spr->fillRect(cx - 12, cy - 6, 9, 3, CLR_WHITE);
    _spr->fillRect(cx + 3,  cy - 6, 9, 3, CLR_WHITE);

    // Desk surface
    _spr->fillRect(cx - 32, cy + 22, 64, 5, CLR_CHROME);
    _spr->drawRect(cx - 32, cy + 22, 64, 5, CLR_DIM);

    // Monitor
    _spr->fillRect(cx - 16, cy - 2, 20, 16, CLR_CHROME);
    _spr->fillRect(cx - 14, cy,     16, 12, CLR_NAVY);
    _spr->drawRect(cx - 16, cy - 2, 20, 16, CLR_DIM);
    // Monitor stand
    _spr->fillRect(cx - 4, cy + 14, 8, 8, CLR_CHROME);

    // Scrolling code on screen
    int scroll = (frame / 3) % 10;
    _spr->drawLine(cx - 12, cy + 2 + (scroll % 4),     cx - 4, cy + 2 + (scroll % 4),     CLR_GREEN);
    _spr->drawLine(cx - 12, cy + 5 + (scroll % 4),     cx - 8, cy + 5 + (scroll % 4),     CLR_DIMCYAN);
    _spr->drawLine(cx - 12, cy + 8 + (scroll % 4) - 4, cx - 6, cy + 8 + (scroll % 4) - 4, CLR_YELLOW);

    // Coffee cup
    _spr->fillRect(cx + 12, cy + 10, 9, 10, CLR_CHROME);
    _spr->drawRect(cx + 12, cy + 10, 9, 10, CLR_DIM);
    _spr->drawLine(cx + 21, cy + 13, cx + 24, cy + 13, CLR_DIM);
    _spr->drawLine(cx + 24, cy + 13, cx + 24, cy + 17, CLR_DIM);
    _spr->drawLine(cx + 24, cy + 17, cx + 21, cy + 17, CLR_DIM);
    // Steam
    if (frame % 8 < 4) {
        _spr->drawLine(cx + 15, cy + 8,  cx + 14, cy + 4, CLR_DIM);
        _spr->drawLine(cx + 18, cy + 8,  cx + 19, cy + 4, CLR_DIM);
    }
}

void Mascot::_drawAlert(int frame) {
    int cx = 45, cy = 60;
    // Flash yellow on packet receive
    uint16_t bodyColor = (frame % 4 < 2) ? CLR_WHITE : 0xFFF0;
    _ghostBody(cx, cy, bodyColor);
    _eyes(cx, cy, true, false, false, CLR_BLACK);
    _mouth(cx, cy, 3);
    // Arms up
    _spr->fillEllipse(cx - 26, cy - 6, 7, 5, CLR_WHITE);
    _spr->fillEllipse(cx + 26, cy - 6, 7, 5, CLR_WHITE);
    // Packet being caught
    if (frame % 6 < 3) {
        _spr->fillRect(cx - 10, cy - 30, 20, 12, CLR_YELLOW);
        _spr->drawRect(cx - 10, cy - 30, 20, 12, CLR_BLACK);
        _spr->drawLine(cx - 8,  cy - 24, cx - 4, cy - 22, CLR_BLACK);
        _spr->drawLine(cx - 4,  cy - 22, cx + 8, cy - 28, CLR_BLACK);
    }
}

void Mascot::_drawTransmit(int frame) {
    int cx = 42, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    _eyes(cx, cy, false, false, false, CLR_BLACK);
    _mouth(cx, cy, 3);
    // Arms cupped
    _spr->fillEllipse(cx - 22, cy + 4, 7, 5, CLR_WHITE);
    _spr->fillEllipse(cx + 22, cy + 4, 7, 5, CLR_WHITE);
    _soundWaves(cx, cy, frame);
}

void Mascot::_drawLowBattery(int frame) {
    int cx = 45, cy = 58;
    uint16_t bodyColor = (frame % 6 < 1) ? CLR_DIM : CLR_WHITE;
    _ghostBody(cx, cy, bodyColor);
    _eyes(cx, cy, false, false, true, CLR_DIM);
    _mouth(cx, cy, 2);

    // Sweat drop
    _spr->fillCircle(cx + 20, cy - 10, 4, CLR_CYAN);
    _spr->fillTriangle(cx + 20, cy - 18, cx + 16, cy - 10, cx + 24, cy - 10, CLR_CYAN);

    // Battery icon
    _spr->drawRect(cx - 10, cy - 45, 20, 10, CLR_RED);
    _spr->fillRect(cx + 10, cy - 43, 3,   6, CLR_RED);
    // Low fill
    _spr->fillRect(cx - 8, cy - 43, 4, 6, CLR_RED);
}

void Mascot::_drawError(int frame) {
    int cx = 45, cy = 58;
    _ghostBody(cx, cy, 0xF008);  // red tint white
    // X eyes
    _spr->drawLine(cx - 12, cy - 8, cx - 5,  cy - 1, CLR_RED);
    _spr->drawLine(cx - 5,  cy - 8, cx - 12, cy - 1, CLR_RED);
    _spr->drawLine(cx + 5,  cy - 8, cx + 12, cy - 1, CLR_RED);
    _spr->drawLine(cx + 12, cy - 8, cx + 5,  cy - 1, CLR_RED);
    _mouth(cx, cy, 2);
    // Facepalm arm
    _spr->fillEllipse(cx + 8, cy - 12, 18, 7, CLR_WHITE);
    // Exclamation
    _spr->fillRect(cx - 30, cy - 44, 6, 16, CLR_RED);
    _spr->fillCircle(cx - 27, cy - 24, 3, CLR_RED);
}

void Mascot::_drawPreflight(int frame) {
    int cx = 44, cy = 58;
    _ghostBody(cx, cy, CLR_WHITE);
    _eyes(cx, cy, false, false, false, CLR_BLACK);
    _mouth(cx, cy, 0);

    // Clipboard
    _spr->fillRect(cx + 8, cy - 12, 22, 28, CLR_CHROME);
    _spr->drawRect(cx + 8, cy - 12, 22, 28, CLR_DIM);
    // Clip at top
    _spr->fillRect(cx + 14, cy - 15, 10, 6, CLR_DIM);

    // Checklist items
    _spr->drawLine(cx + 16, cy - 4, cx + 26, cy - 4, CLR_DIM);
    _spr->drawLine(cx + 16, cy + 2, cx + 26, cy + 2, CLR_DIM);
    _spr->drawLine(cx + 16, cy + 8, cx + 26, cy + 8, CLR_DIM);

    // Animated checkmarks
    int checks = (frame / 25) % 4;
    if (checks > 0) {
        _spr->drawLine(cx + 10, cy - 6,  cx + 13, cy - 3, CLR_GREEN);
        _spr->drawLine(cx + 13, cy - 3,  cx + 16, cy - 8, CLR_GREEN);
    }
    if (checks > 1) {
        _spr->drawLine(cx + 10, cy,     cx + 13, cy + 3,  CLR_GREEN);
        _spr->drawLine(cx + 13, cy + 3, cx + 16, cy - 2,  CLR_GREEN);
    }
    if (checks > 2) {
        _spr->drawLine(cx + 10, cy + 6, cx + 13, cy + 9,  CLR_YELLOW);
        _spr->drawLine(cx + 13, cy + 9, cx + 16, cy + 4,  CLR_YELLOW);
    }

    // Pen in hand
    _spr->drawLine(cx + 6,  cy + 12, cx + 12, cy + 5, CLR_YELLOW);
    _spr->fillCircle(cx + 6, cy + 13, 2, CLR_YELLOW);
}


