

// =============================================================================
// Mascot_LVGL.h  —  LVGL-native mascot
// =============================================================================
//
// USAGE
// ─────
//   MascotLVGL mascot;
//   mascot.begin(parentWidget);          // call once after lv_init()
//   // In your animation timer / task:
//   mascot.draw(0, THEME_STATUS_H, state, frame);
//   // Trigger one-shot action sequences:
//   mascot.triggerAction(MASCOT_PWNAGOTCHI);
//
// LVGL VERSION: v9  (lv_layer_t canvas API)
// =============================================================================

#pragma once
#include <lvgl.h>
#include "MascotState.h"
#include "Theme.h"

// Fixed mascot canvas dimensions
#define SPR_FULL_W   90
#define SPR_FULL_H   110
#define SPR_SMALL_W  60
#define SPR_SMALL_H  90
#define MASCOT_ANIM_MS  120

class MascotLVGL {
public:
    // Create LVGL canvas widgets as children of `parent`.
    // Pixel buffers allocated in PSRAM. Call after lv_init().
    bool begin(lv_obj_t* parent);
    void end();

    // Redraw full mascot at (screenX, screenY). Call every MASCOT_ANIM_MS.
    void draw(int screenX, int screenY, MascotState state, int frame);

    // Redraw small corner widget. Used for status overlay modes.
    void drawSmall(int screenX, int screenY, MascotState state, int frame);

    // Map mode enum to mascot state (same logic as original)
    MascotState stateFromMode(int mode);

    // One-shot action triggers
    void triggerAlert()    { triggerAction(MASCOT_ALERT);    }
    void triggerTransmit() { triggerAction(MASCOT_TRANSMIT); }

    // v2: trigger a one-shot action animation for a state.
    // e.g. call on handshake capture, USB insert, packet receive.
    void triggerAction(MascotState s) {
        if (_animPhase[s] == 0) { _animPhase[s] = 1; _animPhaseFr[s] = 0; }
    }
    void resetAction(MascotState s) {
        _animPhase[s] = 0; _animPhaseFr[s] = 0;
    }

private:
    lv_obj_t* _canvas   = nullptr;
    lv_obj_t* _canvasSm = nullptr;
    void*     _buf      = nullptr;   // PSRAM pixel buffer (full)
    void*     _bufSm    = nullptr;   // PSRAM pixel buffer (small)

    // ── Full-sprite draw methods ─────────────────────────────
    void _drawBoot      (lv_layer_t* l, int frame);
    void _drawStandby   (lv_layer_t* l, int frame);
    void _drawReconWalk (lv_layer_t* l, int frame);
    void _drawWifiRecon (lv_layer_t* l, int frame);
    void _drawLoraRecon (lv_layer_t* l, int frame);
    void _drawPwny      (lv_layer_t* l, int frame);
    void _drawBadUSB    (lv_layer_t* l, int frame);
    void _drawHomelab   (lv_layer_t* l, int frame);
    void _drawAlert     (lv_layer_t* l, int frame);
    void _drawTransmit  (lv_layer_t* l, int frame);
    void _drawLowBattery(lv_layer_t* l, int frame);
    void _drawError     (lv_layer_t* l, int frame);
    void _drawPreflight (lv_layer_t* l, int frame);

    // ── Shared body parts ────────────────────────────────────
    void _ghostBody  (lv_layer_t* l, int cx, int cy, lv_color_t color);
    void _eyes       (lv_layer_t* l, int cx, int cy, bool wide, bool angry,
                      bool halfClosed, lv_color_t color, int shift = 0);
    void _mouth      (lv_layer_t* l, int cx, int cy, int type);
    void _headphones (lv_layer_t* l, int cx, int cy, int frame);
    void _satDish    (lv_layer_t* l, int cx, int cy, int frame);
    void _wifiRings  (lv_layer_t* l, int cx, int cy, int frame);
    void _soundWaves (lv_layer_t* l, int cx, int cy, int frame);
    void _sparks     (lv_layer_t* l, int cx, int cy, int frame);
    void _dataTrail  (lv_layer_t* l, int cx, int cy, int frame);
    void _glasses    (lv_layer_t* l, int cx, int cy);
    void _trenchcoat (lv_layer_t* l, int cx, int cy, int frame);
    void _helmet     (lv_layer_t* l, int cx, int cy);

    // ── Viking / Pwnagotchi props ─────────────────────────────
    void _vikingHorn  (lv_layer_t* l, int cx, int cy, int dir); // dir: -1=L +1=R
    void _vikingHelmet(lv_layer_t* l, int cx, int cy);
    void _furCloak    (lv_layer_t* l, int cx, int cy);
    void _vikingBeard (lv_layer_t* l, int cx, int cy);
    void _battleAxe   (lv_layer_t* l, int cx, int cy, int yOff);
    void _roundShield (lv_layer_t* l, int cx, int cy);
    void _mouthFangs  (lv_layer_t* l, int cx, int cy);

    // ── Recon props ──────────────────────────────────────────
    void _satDishLeft(lv_layer_t* l, int cx, int cy, int frame);
    void _notepad    (lv_layer_t* l, int cx, int cy, int frame);

    // ── Small sprite ─────────────────────────────────────────
    void _smallGhostBody(lv_layer_t* l, int cx, int cy, lv_color_t color);
    void _smallEyes     (lv_layer_t* l, int cx, int cy, lv_color_t color);

    // ── Phase state machine ──────────────────────────────────
    // Phase 0 = idle loop. Phases 1..N = one-shot action sequence.
    // triggerAction() sets phase to 1. Advances automatically each frame.
    int _advancePhase(MascotState s, const uint16_t* dur, int nPhases);
    uint8_t  _animPhase[MASCOT_COUNT]   = {};
    uint16_t _animPhaseFr[MASCOT_COUNT] = {};
};


