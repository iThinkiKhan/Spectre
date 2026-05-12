
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "../config.h"
#include "MascotState.h"
#include "Theme.h"

// Sprite canvas sizes
#define SPR_FULL_W   90
#define SPR_FULL_H   110
#define SPR_SMALL_W  60
#define SPR_SMALL_H  90


// Legacy TFT sprite mascot.
// Scheduled for full LVGL replacement.
// Do not expand this implementation.


class Mascot {
public:
    bool begin(TFT_eSPI* tft);
    void end();

    // Draw full size sprite at screen coords
    void draw(int screenX, int screenY, MascotState state, int frame);

    // Draw small corner widget at screen coords
    void drawSmall(int screenX, int screenY, MascotState state, int frame);

    // Clear sprite area on screen
    void clearFull(int screenX, int screenY);
    void clearSmall(int screenX, int screenY);

    void triggerAlert()    { _alertActive = true;    _alertStart = millis(); }
    void triggerTransmit() { _transmitActive = true; _transmitStart = millis(); }

private:
    TFT_eSPI*    _tft    = nullptr;
    TFT_eSprite* _spr    = nullptr;  // full size sprite
    TFT_eSprite* _sprSm  = nullptr;  // small sprite

    // Center coordinates within the sprite canvas
    // Full sprite: center at (45, 60)
    // Small sprite: center at (18, 28)

    // Full sprite draw methods — draw into _spr
    void _drawBoot(int frame);
    void _drawStandby(int frame);
    void _drawReconWalk(int frame);
    void _drawWifiRecon(int frame);
    void _drawLoraRecon(int frame);
    void _drawPwny(int frame);
    void _drawBadUSB(int frame);
    void _drawHomelab(int frame);
    void _drawAlert(int frame);
    void _drawTransmit(int frame);
    void _drawLowBattery(int frame);
    void _drawError(int frame);
    void _drawPreflight(int frame);

    // Shared body parts — draw into _spr at center cx,cy
    void _ghostBody(int cx, int cy, uint16_t color);
    void _eyes(int cx, int cy, bool wide, bool angry, bool halfClosed, uint16_t color);
    void _mouth(int cx, int cy, int type); // 0=neutral 1=grin 2=frown 3=open
    void _helmet(int cx, int cy);
    void _headphones(int cx, int cy, int frame);
    void _satDish(int cx, int cy, int frame);
    void _devilHorns(int cx, int cy);
    void _claws(int cx, int cy, int frame);
    void _glasses(int cx, int cy);
    void _trenchcoat(int cx, int cy, int frame);
    void _wifiRings(int cx, int cy, int frame);
    void _soundWaves(int cx, int cy, int frame);
    void _sparks(int cx, int cy, int frame);
    void _dataTrail(int cx, int cy, int frame);

    // Small sprite shared parts
    void _smallGhostBody(int cx, int cy, uint16_t color);
    void _smallEyes(int cx, int cy, uint16_t color);

    bool _alertActive    = false;
    bool _transmitActive = false;
    unsigned long _alertStart    = 0;
    unsigned long _transmitStart = 0;
};

#define MASCOT_ANIM_MS  120


