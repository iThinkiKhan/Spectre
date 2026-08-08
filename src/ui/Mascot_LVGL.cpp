
// LVGL v9 canvas mascot. lv_draw_* primitives via lv_layer_t; no TFT/sprites.

#include "Mascot_LVGL.h"
#include "../config.h"
#include <esp_heap_caps.h>
#include <math.h>

// Viking / Pwnagotchi prop colours (not in Theme.h)
#define CLR_BONE        0xE8D0A0
#define CLR_BONE_SHD    0xA09040
#define CLR_BEARD       0xD4A830
#define CLR_FUR         0x5A3A1A
#define CLR_FUR_DK      0x3A2008
#define CLR_STEEL       0x888888
#define CLR_STEEL_DK    0x666666
#define CLR_NOSEGUARD   0x777777
#define CLR_WOOD        0x8B6914
#define CLR_SILVER      0xAAAAAA


// Short-name lv_draw_* wrappers operating on lv_layer_t*.
static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

static void FR(lv_layer_t* l, int x, int y, int w, int h, lv_color_t c) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = c; d.bg_opa = LV_OPA_COVER;
    d.radius = 0; d.border_width = 0;
    lv_area_t a = {(int32_t)x, (int32_t)y,
                   (int32_t)(x+w-1), (int32_t)(y+h-1)};
    lv_draw_rect(l, &d, &a);
}

static void FC(lv_layer_t* l, int cx, int cy, int r, lv_color_t c) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = c; d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE; d.border_width = 0;
    lv_area_t a = {(int32_t)(cx-r), (int32_t)(cy-r),
                   (int32_t)(cx+r), (int32_t)(cy+r)};
    lv_draw_rect(l, &d, &a);
}

static void DC(lv_layer_t* l, int cx, int cy, int r, lv_color_t c, int w = 1) {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = c; d.width = (uint16_t)w; d.opa = LV_OPA_COVER;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(l, &d);
}

static void DA(lv_layer_t* l, int cx, int cy, int r, int sa, int ea, lv_color_t c, int w = 1) {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = c; d.width = (uint16_t)w; d.opa = LV_OPA_COVER;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = sa;
    d.end_angle = ea;
    lv_draw_arc(l, &d);
}

static void DL(lv_layer_t* l, int x1, int y1, int x2, int y2, lv_color_t c, int w = 1) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = c; d.width = (uint16_t)w; d.opa = LV_OPA_COVER;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(l, &d);
}

static void FT(lv_layer_t* l,
               int x1,int y1, int x2,int y2, int x3,int y3, lv_color_t c) {
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.color = c; d.opa = LV_OPA_COVER;
    d.p[0].x = x1; d.p[0].y = y1;
    d.p[1].x = x2; d.p[1].y = y2;
    d.p[2].x = x3; d.p[2].y = y3;
    lv_draw_triangle(l, &d);
}

// Filled ellipse approximation.
// The first uploaded LVGL version drew one rect per scanline, which generated
// too many draw tasks for the ESP32 display loop. A rounded rect is close
// enough at mascot scale and keeps each ellipse to one LVGL primitive.
static void FE(lv_layer_t* l, int cx, int cy, int rx, int ry, lv_color_t c) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = c; d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE; d.border_width = 0;
    lv_area_t a = {(int32_t)(cx-rx), (int32_t)(cy-ry),
                   (int32_t)(cx+rx), (int32_t)(cy+ry)};
    lv_draw_rect(l, &d, &a);
}

// Stroked ellipse approximation, kept intentionally cheap.
static void DE(lv_layer_t* l, int cx, int cy, int rx, int ry, lv_color_t c) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c;
    d.border_opa = LV_OPA_COVER;
    d.border_width = 1;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = {(int32_t)(cx-rx), (int32_t)(cy-ry),
                   (int32_t)(cx+rx), (int32_t)(cy+ry)};
    lv_draw_rect(l, &d, &a);
}

static void TXT(lv_layer_t* l, int x, int y, int maxW, lv_color_t c, const char* txt) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.color = c; d.opa = LV_OPA_COVER;
    d.font = FONT_SMALL;
    d.text = txt;
    lv_area_t a = {(int32_t)x, (int32_t)y,
                   (int32_t)(x + maxW), (int32_t)(y + 16)};
    lv_draw_label(l, &d, &a);
}


// ── LIFECYCLE ──

bool MascotLVGL::begin(lv_obj_t* parent) {
    size_t fullSz = (size_t)SPR_FULL_W  * SPR_FULL_H  * sizeof(lv_color_t);
    size_t smSz   = (size_t)SPR_SMALL_W * SPR_SMALL_H * sizeof(lv_color_t);

    _buf   = heap_caps_malloc(fullSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _bufSm = heap_caps_malloc(smSz,   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!_buf || !_bufSm) {
        if (_buf)   { heap_caps_free(_buf);   _buf   = nullptr; }
        if (_bufSm) { heap_caps_free(_bufSm); _bufSm = nullptr; }
        LV_LOG_ERROR("[MASCOT] PSRAM alloc failed");
        return false;
    }

    _canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(_canvas, _buf,
                         SPR_FULL_W, SPR_FULL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_fill_bg(_canvas, lv_color_hex(CLR_BLACK), LV_OPA_COVER);

    _canvasSm = lv_canvas_create(parent);
    lv_canvas_set_buffer(_canvasSm, _bufSm,
                         SPR_SMALL_W, SPR_SMALL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_clear_flag(_canvasSm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_canvasSm, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_fill_bg(_canvasSm, lv_color_hex(CLR_BLACK), LV_OPA_COVER);

    LV_LOG_INFO("[MASCOT] LVGL canvas ready (PSRAM)");
    return true;
}

void MascotLVGL::end() {
    if (_canvas)   { lv_obj_del(_canvas);   _canvas   = nullptr; }
    if (_canvasSm) { lv_obj_del(_canvasSm); _canvasSm = nullptr; }
    if (_buf)      { heap_caps_free(_buf);   _buf      = nullptr; }
    if (_bufSm)    { heap_caps_free(_bufSm); _bufSm    = nullptr; }
}

void MascotLVGL::draw(int screenX, int screenY, MascotState state, int frame) {
    if (!_canvas) return;
    _frame = frame;
    // One-draw blink every ~26 redraws (≈6-7s at the mascot cadence).
    _blink = (_blinkTick++ % 26) == 25;
    // Idle glance: hold centre most of the cycle, then drift left/right for a
    // few draws. Cheap, but it stops the stare and reads as "thinking".
    const int gazeCycle = _blinkTick % 96;
    _gaze = (gazeCycle < 62) ? 0 : (gazeCycle < 74) ? -2 : (gazeCycle < 86) ? 2 : 0;
    if (_canvasSm) lv_obj_add_flag(_canvasSm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(_canvas, screenX, screenY);
    lv_canvas_fill_bg(_canvas, lv_color_hex(CLR_BLACK), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(_canvas, &layer);

    switch (state) {
        case MASCOT_BOOT_ATTENTION: _drawBoot      (&layer, frame); break;
        case MASCOT_STANDBY:        _drawStandby   (&layer, frame); break;
        case MASCOT_RECON_WALK:     _drawReconWalk (&layer, frame); break;
        case MASCOT_WIFI_RECON:     _drawWifiRecon (&layer, frame); break;
        case MASCOT_LORA_RECON:     _drawLoraRecon (&layer, frame); break;
        case MASCOT_PWNAGOTCHI:     _drawPwny      (&layer, frame); break;
        case MASCOT_BAD_USB:        _drawBadUSB    (&layer, frame); break;
        case MASCOT_HOMELAB_SYNC:   _drawHomelab   (&layer, frame); break;
        case MASCOT_ALERT:          _drawAlert     (&layer, frame); break;
        case MASCOT_TRANSMIT:       _drawTransmit  (&layer, frame); break;
        case MASCOT_MESHTASTIC:     _drawMeshtastic(&layer, frame); break;
        case MASCOT_UPLINK:         _drawUplink    (&layer, frame); break;
        case MASCOT_LOW_BATTERY:    _drawLowBattery(&layer, frame); break;
        case MASCOT_ERROR:          _drawError     (&layer, frame); break;
        case MASCOT_PREFLIGHT:      _drawPreflight (&layer, frame); break;
        default: break;
    }

    lv_canvas_finish_layer(_canvas, &layer);
}

void MascotLVGL::drawSmall(int screenX, int screenY, MascotState state, int frame) {
    if (!_canvasSm) return;
    lv_obj_remove_flag(_canvasSm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(_canvasSm, screenX, screenY);
    lv_canvas_fill_bg(_canvasSm, lv_color_hex(CLR_BLACK), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(_canvasSm, &layer);

    int cx = SPR_SMALL_W / 2, cy = SPR_SMALL_H / 2 - 6;
    lv_color_t bodyColor = C(CLR_WHITE);
    if (state == MASCOT_PWNAGOTCHI)  bodyColor = C(CLR_HOTPINK);
    if (state == MASCOT_ERROR)       bodyColor = C(CLR_RED);
    if (state == MASCOT_LOW_BATTERY && frame % 6 < 1) bodyColor = C(CLR_DIM);

    _smallGhostBody(&layer, cx, cy, bodyColor);

    lv_color_t eyeColor = (state == MASCOT_PWNAGOTCHI) ? C(CLR_RED) : C(CLR_BLACK);
    _smallEyes(&layer, cx, cy, eyeColor);

    if (state == MASCOT_RECON_WALK || state == MASCOT_WIFI_RECON || state == MASCOT_LORA_RECON) {
        lv_color_t dot = (frame % 4 < 2) ? C(CLR_YELLOW) : C(CLR_DIMYELLOW);
        FC(&layer, cx, cy - 20, 3, dot);
    }
    if (state == MASCOT_STANDBY) {
        FR(&layer, cx-7, cy-6, 5, 2, bodyColor);
        FR(&layer, cx+2, cy-6, 5, 2, bodyColor);
    }
    if (state == MASCOT_HOMELAB_SYNC) {
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_DIM); rd.border_width = 1; rd.radius = 0;
        lv_area_t a1 = {(int32_t)(cx-8),(int32_t)(cy-5),(int32_t)(cx-2),(int32_t)(cy-1)};
        lv_area_t a2 = {(int32_t)(cx+2),(int32_t)(cy-5),(int32_t)(cx+8),(int32_t)(cy-1)};
        lv_draw_rect(&layer, &rd, &a1);
        lv_draw_rect(&layer, &rd, &a2);
        DL(&layer, cx-2, cy-3, cx+2, cy-3, C(CLR_DIM));
    }

    lv_canvas_finish_layer(_canvasSm, &layer);
}

int MascotLVGL::_advancePhase(MascotState s, const uint16_t* dur, int nPhases) {
    if (_animPhase[s] == 0) return 0;
    _animPhaseFr[s]++;
    int ph = _animPhase[s];
    if (ph > 0 && ph <= nPhases && _animPhaseFr[s] >= dur[ph - 1]) {
        _animPhaseFr[s] = 0;
        _animPhase[s]++;
        if (_animPhase[s] > nPhases) _animPhase[s] = 0;
    }
    return _animPhase[s];
}


// ── SHARED BODY PARTS ──

// Idle bob: a slow 4-step triangle wave (0,+1,0,-1) rather than a 2-state
// flip, so the float eases instead of ticking between two positions.
int MascotLVGL::_bob() const {
    switch ((_frame / 4) % 4) {
        case 1:  return 1;
        case 3:  return -1;
        default: return 0;
    }
}

// Soft contact shadow. Shrinks as the ghost rises, which sells the hover far
// more cheaply than any change to the body itself.
void MascotLVGL::_groundShadow(lv_layer_t* l, int cx, int bob) {
    // Sits at +50 rather than +44: the longer hem now reaches ~+33, and the
    // body's clearing mask extends below that, which was erasing the shadow.
    const int rx = 15 - bob * 2;
    FE(l, cx, MASCOT_CY + 50, rx, 3, C(0x101010));
    FE(l, cx, MASCOT_CY + 50, rx - 5, 2, C(0x1C1C1C));
}

// Travelling wave. Amplitude is deliberately 1px and the phase advances every
// fifth frame: at amplitude 2 with a faster phase the bands snaked far enough
// out of alignment that the ghost looked like it was dancing rather than
// drifting. Note the mascot only redraws every ~260ms while `frame` ticks with
// the 70ms display loop, so `frame` jumps ~4 per redraw — the divisor has to
// absorb that or the wave advances a full step every single draw.
int MascotLVGL::_wave(int step) const {
    static const int8_t kWave[8] = { 0, 0, 1, 1, 0, 0, -1, -1 };
    return kWave[(unsigned)((_frame / 5) + step) & 7u];
}

// Elongation cycle: the ghost draws itself up and settles back, 0..3px.
int MascotLVGL::_stretch() const {
    static const int8_t kStretch[6] = { 0, 1, 2, 3, 2, 1 };
    return kStretch[((unsigned)(_frame / 6)) % 6u];
}

void MascotLVGL::_ghostBody(lv_layer_t* l, int cx, int cy, lv_color_t color) {
    const int rx = MASCOT_BODY_RX;   // 19 -> body spans 38px, fits 72 with margin
    const int stretch = _stretch();
    // Hem sits level with the head dome's lowest point (cy+21). Any higher and
    // the dome's bottom arc pokes out past the torso bands, reading as a hard
    // jawline and breaking the "sheet with no skeleton" illusion; this is the
    // shortest the body can be while still burying that curve.
    const int hemY = cy + 21 + stretch;

    // Head rides the crest of the wave.
    const int headOff = _wave(0);
    FE(l, cx + headOff, cy, rx, 21, color);

    // Torso drawn as stacked bands rather than one rectangle. Each band is
    // offset by the next step of the wave, so the silhouette ripples down the
    // whole body instead of only the hem. Band height 4 keeps this to ~4 draw
    // calls — a per-scanline version was tried early on and swamped the ESP32
    // draw pipeline, so the slicing stays deliberately coarse.
    int step = 1;
    for (int y = cy + 6; y < hemY; y += 4, ++step) {
        const int h = LV_MIN(4, hemY - y);
        FR(l, cx - rx + _wave(step), y, rx * 2, h, color);
    }

    // Hem scallops continue the same wave so the ripple resolves at the bottom.
    // Radius 7 at 12px spacing means neighbours overlap by 2px, so the notches
    // between bumps fall out of the geometry (~3.5px deep) on their own. The
    // original design punched extra black circles in here to deepen those
    // notches, but at this spacing they cut voids that read as holes rather
    // than as hem — so the scallops are now left to form the edge themselves.
    for (int i = 0; i < 3; ++i) {
        FC(l, cx - 12 + i * 12 + _wave(step + i), hemY, 7, color);
    }
    FR(l, cx - rx - 3, hemY + 8, rx * 2 + 6, 8, C(CLR_BLACK));

    // No outline on the head. The original full-ellipse stroke drew a jawline
    // across the body, and replacing it with a top-only arc was no better:
    // lv_draw_arc is strictly circular while the head is an ellipse (rx 19,
    // ry 21), so any arc that matches at the sides sits inside the dome at the
    // crown and paints a visible curve across the white. White-on-black is
    // already high contrast, so the silhouette carries the shape by itself.
}

void MascotLVGL::_eyes(lv_layer_t* l, int cx, int cy,
                   bool wide, bool angry, bool halfClosed,
                   lv_color_t color, int shift) {
    cx += _wave(0);   // ride the head band so the face stays attached
    // Idle glance rides on top of any caller-supplied shift, so a state that
    // deliberately looks at a prop keeps its direction and just drifts a little.
    if (shift == 0) shift = _gaze;

    // Natural blink: for one draw every ~6-7s, drop the eyes to lids so the
    // ghost never reads as frozen. Skipped when the caller already draws a
    // lidded/half-closed expression. Costs nothing — rides the redraw.
    if (!halfClosed && _blink) {
        DL(l, cx-11+shift, cy-4, cx-3+shift, cy-4, C(CLR_DIM), 2);
        DL(l, cx+3+shift,  cy-4, cx+11+shift, cy-4, C(CLR_DIM), 2);
        return;
    }
    int r = wide ? 4 : 3;
    FC(l, cx-7+shift, cy-4, r, color);
    FC(l, cx+7+shift, cy-4, r, color);
    // Specular catchlight — the single cheapest thing that makes eyes read as
    // alive rather than as flat drilled holes.
    if (!halfClosed) {
        FC(l, cx-8+shift, cy-5, 1, C(CLR_WHITE));
        FC(l, cx+6+shift, cy-5, 1, C(CLR_WHITE));
    }
    if (halfClosed) {
        FR(l, cx-12, cy-8, 10, 4, C(CLR_WHITE));
        FR(l, cx+2,  cy-8, 10, 4, C(CLR_WHITE));
    }
    if (angry) {
        DL(l, cx-12, cy-9, cx-3, cy-7, C(CLR_BLACK));
        DL(l, cx+3,  cy-7, cx+12, cy-9, C(CLR_BLACK));
    }
}

void MascotLVGL::_mouth(lv_layer_t* l, int cx, int cy, int type) {
    cx += _wave(0);   // ride the head band so the face stays attached
    lv_color_t dim = C(CLR_DIM);
    switch (type) {
        case 0: DL(l, cx-5, cy+8, cx+5, cy+8, dim); break;
        case 1: // grin
            FR(l, cx-7, cy+6, 14, 6, C(CLR_BLACK));
            FR(l, cx-5, cy+7, 4,  4, C(CLR_WHITE));
            FR(l, cx,   cy+7, 4,  4, C(CLR_WHITE));
            FR(l, cx+5, cy+7, 3,  3, C(CLR_WHITE));
            break;
        case 2: // frown
            DL(l, cx-5, cy+10, cx,   cy+7, dim);
            DL(l, cx,   cy+7,  cx+5, cy+10, dim);
            break;
        case 3: // open
            FE(l, cx, cy+8, 5, 4, dim);
            break;
    }
}

void MascotLVGL::_headphones(lv_layer_t* l, int cx, int cy, int frame) {
    cx += _wave(0);   // ride the head band so the face stays attached
    lv_color_t cyan = C(CLR_CYAN), dcyan = C(CLR_DIMCYAN);
    DA(l, cx, cy-20, 22, 200, 340, cyan, 4);
    FC(l, cx-21, cy-10, 7, cyan);
    FC(l, cx+21, cy-10, 7, cyan);
    FC(l, cx-21, cy-10, 4, dcyan);
    FC(l, cx+21, cy-10, 4, dcyan);
    if (frame % 6 < 3) {
        DC(l, cx-21, cy-10, 9,  cyan);
        DC(l, cx+21, cy-10, 9,  cyan);
    }
    if (frame % 6 < 1) {
        DC(l, cx-21, cy-10, 11, dcyan);
        DC(l, cx+21, cy-10, 11, dcyan);
    }
}

// Compact dish tucked into the right margin. Was at cx+32 with a 10px radius
// (reaching canvas x=78) which the 72px panel cut in half.
void MascotLVGL::_satDish(lv_layer_t* l, int cx, int cy, int frame) {
    int dx = cx+23, dy = cy+6;
    DL(l, dx, dy, dx, dy-13, C(CLR_DIM));
    FE(l, dx, dy-15, 7, 4, C(CLR_DIM));
    DL(l, dx, dy-19, dx-5, dy-24, C(CLR_DIM));
    int ph = frame % 8;
    if (ph > 0) DA(l, dx-6, dy-26, 4,  250, 20, C(CLR_YELLOW));
    if (ph > 2) DA(l, dx-6, dy-26, 8,  250, 20, C(CLR_DIMYELLOW));
    if (ph > 4) DA(l, dx-6, dy-26, 12, 250, 20, C(CLR_CHROME));
}

// Handheld scanner + emitted rings. The rings were full circles centred at
// cx+38 with r=15 (canvas x up to 89) — almost entirely outside the panel.
// They are now right-facing arcs anchored at the scanner so the whole sweep
// stays inside the safe area.
void MascotLVGL::_wifiRings(lv_layer_t* l, int cx, int cy, int frame) {
    const int sx = cx + 14;              // scanner body left edge
    FR(l, sx, cy-13, 9, 13, C(CLR_CHROME));
    {   // scanner device outline
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_CYAN); rd.border_width = 1; rd.radius = 0;
        lv_area_t a = {(int32_t)sx,(int32_t)(cy-13),(int32_t)(sx+8),(int32_t)(cy)};
        lv_draw_rect(l, &rd, &a);
    }
    FR(l, sx+2, cy-11, 5, 3, C(CLR_NAVY));
    FE(l, cx+11, cy+2, 6, 4, C(CLR_WHITE));   // hand
    int ph = (frame % 8) + 1;
    const int ax = sx + 5, ay = cy - 15;
    if (ph > 1) DA(l, ax, ay, 5,  250, 20, C(CLR_CYAN));
    if (ph > 3) DA(l, ax, ay, 10, 250, 20, C(CLR_DIMCYAN));
    if (ph > 5) DA(l, ax, ay, 15, 250, 20, C(CLR_CHROME));
}

void MascotLVGL::_soundWaves(lv_layer_t* l, int cx, int cy, int frame) {
    // Anchored at cx+18 with radii <= 14 so the outermost arc lands at x=68,
    // inside the 70px safe edge (was cx+30 r20 -> x=86, clipped).
    int ph = frame % 6;
    DA(l, cx+18, cy, 6,  315, 45, ph > 0 ? C(CLR_YELLOW) : C(CLR_DIMYELLOW));
    DA(l, cx+18, cy, 10, 315, 45, ph > 2 ? C(CLR_YELLOW) : C(CLR_DIMYELLOW));
    DA(l, cx+18, cy, 14, 315, 45, ph > 4 ? C(CLR_DIMYELLOW) : C(CLR_DIM));
}

void MascotLVGL::_sparks(lv_layer_t* l, int cx, int cy, int frame) {
    if (frame % 2 == 0) {
        DL(l, cx+22, cy,   cx+28, cy-7,  C(CLR_YELLOW), 2);
        DL(l, cx+28, cy-7, cx+24, cy-12, C(CLR_ORANGE), 1);
        FC(l, cx+26, cy-4, 2, C(CLR_WHITE));
    } else {
        DL(l, cx+20, cy-2, cx+27, cy-9,  C(CLR_YELLOW), 2);
        DL(l, cx+18, cy-6, cx+25, cy-13, C(CLR_ORANGE), 1);
        FC(l, cx+22, cy-8, 2, C(CLR_WHITE));
    }
}

void MascotLVGL::_dataTrail(lv_layer_t* l, int cx, int cy, int frame) {
    // Trail now runs cx-22 .. cx-property inside the left margin; the old
    // spacing walked out to cx-57 (negative canvas x) and vanished.
    int ph = frame % 12;
    const uint32_t cols[] = { CLR_YELLOW, CLR_DIMYELLOW, CLR_DIM, 0x202020 };
    for (int i = 0; i < 4; i++) {
        int dx = cx - 22 - (i * 4) - (ph < 6 ? 1 : 0);
        if (dx < MASCOT_SAFE_L) continue;
        FC(l, dx, cy+14, i < 2 ? 2 : 1, C(cols[i]));
    }
}

void MascotLVGL::_glasses(lv_layer_t* l, int cx, int cy) {
    cx += _wave(0);   // ride the head band so the face stays attached
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_DIM); rd.border_width = 1; rd.radius = 0;
    lv_area_t a1 = {(int32_t)(cx-14),(int32_t)(cy-7),(int32_t)(cx-2), (int32_t)(cy)};
    lv_area_t a2 = {(int32_t)(cx+2), (int32_t)(cy-7),(int32_t)(cx+14),(int32_t)(cy)};
    lv_draw_rect(l, &rd, &a1);
    lv_draw_rect(l, &rd, &a2);
    DL(l, cx-2, cy-3, cx+2,  cy-3, C(CLR_DIM));
    DL(l, cx-26,cy-3, cx-14, cy-3, C(CLR_DIM));
    DL(l, cx+14,cy-3, cx+26, cy-3, C(CLR_DIM));
}

void MascotLVGL::_trenchcoat(lv_layer_t* l, int cx, int cy, int frame) {
    int sway = frame % 8 < 4 ? 3 : -3;
    lv_color_t chr = C(CLR_CHROME);
    FT(l, cx-22, cy+12, cx-30, cy+38, cx-12, cy+38, chr);
    FT(l, cx+22, cy+12, cx+12, cy+38, cx+30+sway, cy+38, chr);
    DL(l, cx-4, cy+12, cx-10, cy+24, C(CLR_DIM));
    DL(l, cx+4, cy+12, cx+10, cy+24, C(CLR_DIM));
    FC(l, cx, cy+20, 2, C(CLR_DIM));
}

void MascotLVGL::_helmet(lv_layer_t* l, int cx, int cy) {
    cx += _wave(0);   // ride the head band so the face stays attached
    lv_color_t olive = C(0x5DC040), dark = C(0x3A8020);
    FE(l, cx+2, cy-26, 26, 10, olive);
    FR(l, cx-24, cy-28, 50, 8, olive);
    FE(l, cx+2, cy-32, 20, 8, dark);
    DL(l, cx-24, cy-22, cx+26, cy-22, C(0x6B6020));
    DL(l, cx-20, cy-24, cx-24, cy-8,  C(CLR_DIM));
    DL(l, cx-24, cy-8,  cx-18, cy-2,  C(CLR_DIM));
    DL(l, cx+16, cy-24, cx+20, cy-10, C(CLR_DIM));
}

// ─── Viking props ─────────────────────────────────────────────────────────────

// Horns curl to cx±30 instead of cx±44 — the old span was 88px wide on a 72px
// canvas, so both horn tips were amputated by the panel edges.
void MascotLVGL::_vikingHorn(lv_layer_t* l, int cx, int cy, int dir) {
    cx += _wave(0);   // ride the head band so the face stays attached
    lv_color_t bone = C(CLR_BONE), shadow = C(CLR_BONE_SHD);
    int bx = cx + dir*12;
    FT(l, bx,         cy-16, bx,            cy-21, cx+dir*20, cy-24, bone);
    FT(l, bx,         cy-21, cx+dir*20,     cy-24, cx+dir*26, cy-31, bone);
    FT(l, cx+dir*20,  cy-24, cx+dir*26,     cy-31, cx+dir*30, cy-38, bone);
    DL(l, bx+dir*2,  cy-18, cx+dir*22, cy-25, shadow);
    DL(l, cx+dir*22, cy-25, cx+dir*30, cy-38, shadow);
}

void MascotLVGL::_vikingHelmet(lv_layer_t* l, int cx, int cy) {
    cx += _wave(0);   // ride the head band so the face stays attached
    FE(l, cx, cy-26, 24, 12, C(CLR_STEEL));
    FR(l, cx-24, cy-28, 48, 8,  C(CLR_STEEL));
    FE(l, cx, cy-30, 18, 8,  C(CLR_STEEL_DK));
    FR(l, cx-3, cy-26, 6, 12, C(CLR_NOSEGUARD));
    DL(l, cx-24, cy-22, cx+24, cy-22, C(CLR_BONE));  // brim highlight
}

void MascotLVGL::_furCloak(lv_layer_t* l, int cx, int cy) {
    lv_color_t fur = C(CLR_FUR), furDk = C(CLR_FUR_DK);
    FT(l, cx-24, cy+8, cx-31, cy+38, cx+31, cy+38, fur);
    FT(l, cx-24, cy+8, cx+24, cy+8,  cx+31, cy+38, fur);
    for (int i = 0; i < 6; i++) FC(l, cx-20 + i*8, cy+38, 3, furDk);
}

void MascotLVGL::_vikingBeard(lv_layer_t* l, int cx, int cy) {
    cx += _wave(0);   // ride the head band so the face stays attached
    lv_color_t beard = C(CLR_BEARD), wood = C(CLR_WOOD);
    FR(l, cx-14, cy+6, 28, 18, beard);
    FT(l, cx-14, cy+24, cx+14, cy+24, cx, cy+36, beard);
    DL(l, cx-4, cy+8, cx-6, cy+30, wood);
    DL(l, cx+4, cy+8, cx+6, cy+30, wood);
}

void MascotLVGL::_battleAxe(lv_layer_t* l, int cx, int cy, int yOff) {
    // Head ends at cx+32 (canvas x=68) rather than cx+44 (x=80, off-panel).
    FR(l, cx+21, cy-8+yOff, 3, 26, C(CLR_WOOD));
    FT(l, cx+21, cy-8+yOff, cx+32, cy-13+yOff, cx+32, cy-4+yOff,  C(CLR_SILVER));
    FT(l, cx+21, cy-8+yOff, cx+24, cy+1+yOff,  cx+32, cy-4+yOff,  C(CLR_SILVER));
    DL(l, cx+32, cy-13+yOff, cx+32, cy-4+yOff, C(CLR_STEEL));
    DL(l, cx+21, cy-8+yOff,  cx+32, cy-13+yOff, C(CLR_STEEL_DK));
}

void MascotLVGL::_roundShield(lv_layer_t* l, int cx, int cy) {
    // Centre pulled to cx-23 with r=10 so the rim sits at canvas x=3 instead of
    // x=-6, where the left third of the shield was being cut away.
    FC(l, cx-23, cy+4, 10, C(CLR_FUR));
    DC(l, cx-23, cy+4, 10, C(CLR_WOOD), 2);
    DL(l, cx-23, cy-5,  cx-23, cy+13, C(CLR_BEARD));
    DL(l, cx-32, cy+4,  cx-14, cy+4,  C(CLR_BEARD));
    FC(l, cx-23, cy+4, 3, C(CLR_BEARD));
}

void MascotLVGL::_mouthFangs(lv_layer_t* l, int cx, int cy) {
    cx += _wave(0);   // ride the head band so the face stays attached
    FR(l, cx-9, cy+5, 18, 8, C(CLR_BLACK));
    FT(l, cx-8, cy+5, cx-4, cy+13, cx,   cy+5, C(CLR_WHITE));
    FT(l, cx+1, cy+5, cx+5, cy+13, cx+9, cy+5, C(CLR_WHITE));
    FT(l, cx-3, cy+5, cx-1, cy+8,  cx+1, cy+5, C(CLR_DIM));
}

void MascotLVGL::_satDishLeft(lv_layer_t* l, int cx, int cy, int frame) {
    // Mast at cx-25 (canvas x=11); the old cx-42 put it at x=-6, fully clipped.
    int dx = cx-25, dy = cy+8;
    DL(l, dx, dy, dx, dy-15, C(CLR_DIM));
    FE(l, dx, dy-17, 9, 5, C(CLR_DARKGREY));
    DC(l, dx, dy-17, 9, C(CLR_YELLOW));
    DL(l, dx, dy-22, dx-4, dy-26, C(CLR_DIM));
    int ph = frame % 10;
    if (ph > 2) DA(l, dx, dy-19, 6,  150, 210, C(CLR_YELLOW));
    if (ph > 5) DA(l, dx, dy-19, 9,  150, 210, C(CLR_DIMYELLOW));
    if (ph > 8) DA(l, dx, dy-19, 12, 150, 210, C(CLR_DARKGREY));
}

void MascotLVGL::_notepad(lv_layer_t* l, int cx, int cy, int frame) {
    // Pad spans cx+13..cx+30 (canvas 49..66) and the pen tip stops at cx+32,
    // keeping the whole writing gesture inside the panel.
    FR(l, cx+13, cy-2, 17, 21, C(CLR_BONE));
    {
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_BONE_SHD); rd.border_width = 1; rd.radius = 0;
        lv_area_t a = {(int32_t)(cx+13),(int32_t)(cy-2),(int32_t)(cx+29),(int32_t)(cy+18)};
        lv_draw_rect(l, &rd, &a);
    }
    for (int row = 0; row < 4; row++)
        DL(l, cx+15, cy+3+row*5, cx+27, cy+3+row*5, C(CLR_BONE_SHD));
    int lineRow = (frame / 16) % 4;
    int lineLen = LV_MIN(frame % 16, 12);
    DL(l, cx+15, cy+3+lineRow*5, cx+15+lineLen, cy+3+lineRow*5, C(CLR_BLACK));
    int tap = (frame % 8 < 4) ? 1 : 0;
    FE(l, cx+24, cy+13+tap, 5, 4, C(CLR_WHITE));
    DL(l, cx+26, cy+9+tap, cx+31, cy+4+tap, C(CLR_YELLOW), 2);
    FC(l, cx+26, cy+9+tap, 2, C(CLR_YELLOW));
}

void MascotLVGL::_smallGhostBody(lv_layer_t* l, int cx, int cy, lv_color_t color) {
    FE(l, cx, cy, 18, 20, color);
    FR(l, cx-18, cy, 36, 14, color);
    for (int bx : {cx-11, cx, cx+11}) FC(l, bx, cy+14, 8, color);
    FR(l, cx-18, cy+18, 36, 6, C(CLR_BLACK));
    FC(l, cx-6, cy+19, 4, C(CLR_BLACK));
    FC(l, cx+6, cy+19, 4, C(CLR_BLACK));
}

void MascotLVGL::_smallEyes(lv_layer_t* l, int cx, int cy, lv_color_t color) {
    FC(l, cx-6, cy-4, 3, color);
    FC(l, cx+6, cy-4, 3, color);
}


// ── DRAW METHODS ──

// ─── BOOT ─────────────────────────────────────────────────────────────────────
void MascotLVGL::_drawBoot(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _helmet(l, cx, cy);
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);
    if (frame < 10) {
        int armX = cx+14 + frame*2/3;
        int armY = cy+2  - frame*2;
        FE(l, armX, armY, 6, 5, C(CLR_WHITE));
    } else {
        FE(l, cx+20, cy-16, 6, 5, C(CLR_WHITE));
        DL(l, cx+13, cy+2, cx+18, cy-14, C(CLR_WHITE), 2);
    }

    // Subsystem bring-up ticks, so boot reads as work happening rather than a
    // ghost waving at nothing. Four blocks light in sequence and hold.
    const int lit = LV_MIN(frame / 6, 4);
    for (int i = 0; i < 4; ++i) {
        const lv_color_t bc = (i < lit) ? C(CLR_GREEN) : C(0x1A1A1A);
        FR(l, cx - 28 + i * 8, cy + 40, 6, 5, bc);
    }
    if (lit >= 4 && (frame / 3) % 2) {
        DL(l, cx - 28, cy + 48, cx + 2, cy + 48, C(CLR_GREEN));
    }
}

// ─── STANDBY ──────────────────────────────────────────────────────────────────
void MascotLVGL::_drawStandby(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    int cycle = frame % 160;

    if (cycle < 90) {
        int bob = frame % 8 < 4 ? -1 : 1;
        _ghostBody(l, cx, cy+bob, C(CLR_WHITE));
        _headphones(l, cx, cy+bob, frame);
        _eyes(l, cx, cy+bob, false, false, true, C(CLR_BLACK));
        _mouth(l, cx, cy+bob, 0);
        _satDish(l, cx, cy+bob, frame);
    } else if (cycle < 110) {
        int yf = cycle - 90;
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _headphones(l, cx, cy, frame);
        _eyes(l, cx, cy, false, false, true, C(CLR_BLACK));
        FE(l, cx, cy+8, 3 + yf/4, 2 + yf/5, C(CLR_DIM));
        _satDish(l, cx, cy, frame);
        if (yf > 12) FC(l, cx-12, cy+2, 2, C(CLR_DIMCYAN));
    } else if (cycle < 150) {
        _ghostBody(l, cx, cy+2, C(CLR_WHITE));
        _headphones(l, cx, cy+2, 0);
        FR(l, cx-13, cy-6, 11, 6, C(CLR_WHITE));
        FR(l, cx+2,  cy-6, 11, 6, C(CLR_WHITE));
        FE(l, cx+4, cy+10, 2, 2, C(CLR_DIM));
        _satDish(l, cx, cy+2, 0);
    } else {
        int shake = cycle % 2 ? 2 : -2;
        _ghostBody(l, cx+shake, cy, C(CLR_WHITE));
        _headphones(l, cx, cy, frame);
        _eyes(l, cx, cy, true, false, false, C(CLR_BLACK));
        _mouth(l, cx, cy, 3);
    }

    // Vitals monitor the ghost is idly watching. The SYSTEM page behind this
    // state is all health readouts, so give the character something to actually
    // be monitoring — a steady ECG blip reads as "everything nominal" and keeps
    // the relaxed listening-post theme rather than replacing it.
    const int mx = cx - 33, my = cy - 12;      // small screen, left margin
    FR(l, mx, my, 21, 17, C(CLR_NAVY));
    {
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_DIMCYAN);
        rd.border_width = 1; rd.radius = 0;
        lv_area_t a = {(int32_t)mx,(int32_t)my,(int32_t)(mx+20),(int32_t)(my+16)};
        lv_draw_rect(l, &rd, &a);
    }
    const int baseline = my + 11;
    DL(l, mx+2, baseline, mx+18, baseline, C(0x0F3A2A));
    const int beat = (frame / 2) % 8;          // sweep position across the trace
    for (int i = 0; i < 8; ++i) {
        const int x = mx + 2 + i * 2;
        int h = 0;
        if (i == beat)          h = 6;         // R spike
        else if (i == beat + 1) h = -3;        // S dip
        else if (i == beat + 2) h = 2;         // T wave
        if (h) DL(l, x, baseline, x, baseline - h, C(CLR_GREEN));
    }
    FC(l, mx + 17, my + 3, 1, (frame / 4) % 2 ? C(CLR_GREEN) : C(0x0F3A2A));
}

// ─── RECON (formerly ReconWalk) ───────────────────────────────────────────────
void MascotLVGL::_drawReconWalk(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    static const uint16_t PHASE_DUR[] = { 30 };
    int phase = _advancePhase(MASCOT_RECON_WALK, PHASE_DUR, 1);

    if (phase == 0) {
        _satDishLeft(l, cx, cy, frame);
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        FE(l, cx-24, cy-2, 9, 14, C(CLR_WHITE));
        FE(l, cx-24, cy-2, 5,  9, C(CLR_DIM));
        FE(l, cx-24, cy-2, 2,  4, C(CLR_BLACK));
        _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), -3);
        _mouth(l, cx, cy, 0);
        _notepad(l, cx, cy, frame);
    } else {
        int pf = _animPhaseFr[MASCOT_RECON_WALK];
        lv_color_t flash = (pf % 4 < 2) ? C(CLR_YELLOW) : C(CLR_CYAN);
        FE(l, cx-42, cy-10, 12, 6, C(CLR_DARKGREY));
        DC(l, cx-42, cy-10, 12, flash);
        DL(l, cx-42, cy, cx-42, cy-18, C(CLR_DIM));
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        FE(l, cx-24, cy-2, 9, 14, C(CLR_WHITE));
        FE(l, cx-24, cy-2, 5,  9, C(CLR_DIM));
        FE(l, cx-24, cy-2, 2,  4, C(CLR_BLACK));
        _eyes(l, cx, cy, true, false, false, C(CLR_BLACK), -3);
        _mouth(l, cx, cy, 3);
        FR(l, cx+16, cy+4, 18, 22, C(CLR_BONE));
        FR(l, cx+32, cy-42, 5, 14, flash);
        FC(l, cx+34, cy-24, 3, flash);
    }
}

// ─── WIFI RECON ───────────────────────────────────────────────────────────────
// Matches the ENTITIES page (total / nearby / observations / closest entity):
// a surveyor sweeping with a scanner while detected entities pop in around it
// and a running tally ticks up on the left.
void MascotLVGL::_drawWifiRecon(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    const int bob = _bob();
    cy += bob;
    _groundShadow(l, cx, bob);

    // Detected entities: pips fading in at staggered ranges, brightest when
    // freshly seen — the visual echo of "nearby" on the page.
    for (int i = 0; i < 4; ++i) {
        const int ph = (frame / 3 + i * 2) % 8;
        if (ph > 5) continue;
        const int ex = cx - 27 + (i % 2) * 6;
        const int ey = cy - 26 + i * 9;
        const lv_color_t ec = (ph < 2) ? C(CLR_GREEN)
                            : (ph < 4) ? C(CLR_DIMCYAN) : C(0x123033);
        FC(l, ex, ey, (ph < 2) ? 2 : 1, ec);
    }

    _ghostBody(l, cx, cy, C(CLR_WHITE));
    // Focused squint aimed at the scanner rather than a blank forward stare.
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), 2);
    DL(l, cx-11, cy-9, cx-4, cy-7, C(CLR_BLACK));
    DL(l, cx+4,  cy-7, cx+11, cy-9, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);
    _wifiRings(l, cx, cy, frame);

    // Tally marks: four strokes then a diagonal, cycling — the "observations"
    // counter climbing.
    const int tally = (frame / 8) % 6;
    for (int i = 0; i < LV_MIN(tally, 4); ++i) {
        DL(l, cx-33 + i*3, cy+16, cx-33 + i*3, cy+23, C(CLR_DIMYELLOW));
    }
    if (tally >= 5) DL(l, cx-35, cy+23, cx-22, cy+16, C(CLR_YELLOW));
}

// ─── LORA RECON ───────────────────────────────────────────────────────────────
// Matches the SUB-GHZ RECON page (RSSI / SNR / PACKETS / last signal). Its own
// visual language, distinct from the mesh peers and the entity pips: a
// directional yagi sweeping the band, a live spectrum waterfall along the
// bottom, and captured packets sliding down the boom into the ghost's hand.
void MascotLVGL::_drawLoraRecon(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    const int bob = _bob();
    cy += bob;
    _groundShadow(l, cx, bob);

    // Which spectrum bin is "hot" this cycle — the signal being tracked.
    const int hot = (frame / 7) % 7;

    // Incoming wavefront arriving at the antenna, strongest when a packet lands.
    const int ph = frame % 8;
    // Outer radius capped at 15 so the wavefront stops at x=69, clear of the
    // divider (r17 put it at x=71, right on the panel edge).
    if (ph > 0) DA(l, cx+18, cy-26, 6,  200, 340, C(CLR_YELLOW));
    if (ph > 2) DA(l, cx+18, cy-26, 11, 200, 340, C(CLR_DIMYELLOW));
    if (ph > 4) DA(l, cx+18, cy-26, 15, 200, 340, C(CLR_DIM));

    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), 2);
    _mouth(l, cx, cy, 0);

    // Yagi: angled boom with director elements, held up and to the right.
    const int bx0 = cx+9,  by0 = cy+2;    // grip
    const int bx1 = cx+22, by1 = cy-22;   // tip
    DL(l, bx0, by0, bx1, by1, C(CLR_DIM), 2);
    for (int i = 0; i < 4; ++i) {
        const int t  = i + 1;
        const int ex = bx0 + (bx1 - bx0) * t / 5;
        const int ey = by0 + (by1 - by0) * t / 5;
        const int halfLen = 7 - i;        // elements shorten toward the tip
        DL(l, ex - halfLen, ey - 1, ex + halfLen, ey + 1, C(CLR_SILVER));
    }
    FE(l, cx+7, cy+4, 6, 4, C(CLR_WHITE));   // hand on the grip

    // Captured packet sliding down the boom when the hot bin fires.
    if (ph >= 4) {
        const int t  = ph - 4;                       // 0..3
        const int px = bx1 + (bx0 - bx1) * t / 3;
        const int py = by1 + (by0 - by1) * t / 3;
        FR(l, px-2, py-2, 4, 4, C(CLR_GREEN));
    }

    // Spectrum waterfall: 7 bins along the bottom, the hot one spiking.
    const int baseY = cy + 43;   // below the lengthened hem
    for (int i = 0; i < 7; ++i) {
        int h = 2 + ((frame / 2 + i * 3) % 4);       // idle noise floor
        lv_color_t bc = C(CLR_DIMCYAN);
        if (i == hot) { h = 8 + (frame % 2); bc = C(CLR_YELLOW); }
        FR(l, cx - 24 + i * 7, baseY - h, 4, h, bc);
    }
    DL(l, cx - 25, baseY + 1, cx + 25, baseY + 1, C(CLR_DIM));
}

// ─── PWNAGOTCHI — Full Viking ─────────────────────────────────────────────────
void MascotLVGL::_drawPwny(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    static const uint16_t PHASE_DUR[] = { 20, 35, 40 };
    int phase = _advancePhase(MASCOT_PWNAGOTCHI, PHASE_DUR, 3);
    int pf = _animPhaseFr[MASCOT_PWNAGOTCHI];

    auto viking = [&](int vcx, int vcy) {
        _furCloak    (l, vcx, vcy);
        _ghostBody   (l, vcx, vcy, C(CLR_WHITE));
        _vikingHelmet(l, vcx, vcy);
        _vikingHorn  (l, vcx, vcy, -1);
        _vikingHorn  (l, vcx, vcy, +1);
        _vikingBeard (l, vcx, vcy);
        _mouthFangs  (l, vcx, vcy);
    };

    if (phase == 0) {
        int sway = frame % 12 < 6 ? 2 : -2;
        viking(cx, cy);
        _eyes(l, cx, cy, true, true, false, C(CLR_RED));
        _battleAxe(l, cx, cy, sway);
        _roundShield(l, cx, cy);

    } else if (phase == 1) {
        viking(cx, cy);
        _eyes(l, cx, cy, true, true, false, C(CLR_RED), 4);
        FR(l, cx+28, cy-28, 4, 30, C(CLR_WOOD));
        FT(l, cx+28, cy-28, cx+44, cy-36, cx+44, cy-26, C(CLR_SILVER));
        FT(l, cx+28, cy-28, cx+32, cy-18, cx+44, cy-26, C(CLR_SILVER));
        int bx = cx+44 + pf*4;
        if (bx < cx+88) {
            DL(l, bx,   cy-20, bx-8, cy-8,  C(CLR_RED), 3);
            DL(l, bx-8, cy-8,  bx+2, cy-8,  C(CLR_RED), 2);
            DL(l, bx+2, cy-8,  bx-6, cy+4,  C(CLR_RED), 3);
        }

    } else if (phase == 2) {
        int shake = (pf < 10 && pf % 2) ? 2 : 0;
        viking(cx+shake, cy);
        _eyes(l, cx+shake, cy, true, false, false, C(CLR_RED));
        FE(l, cx+shake, cy+8, 9, 8, C(CLR_BLACK));
        FT(l, cx-8+shake, cy+3, cx-4+shake, cy+12, cx+shake,   cy+3, C(CLR_WHITE));
        FT(l, cx+1+shake, cy+3, cx+5+shake, cy+12, cx+9+shake, cy+3, C(CLR_WHITE));
        FE(l, cx-28+shake, cy-10, 7, 5, C(CLR_WHITE));
        FE(l, cx+28+shake, cy-10, 7, 5, C(CLR_WHITE));
        int sp = pf % 8;
        if (sp > 1) DA(l, cx+shake, cy+8, 14, 200, 340, C(CLR_YELLOW));
        if (sp > 4) DA(l, cx+shake, cy+8, 22, 200, 340, C(CLR_DIMYELLOW));

    } else {
        int bounce = pf < 10 ? -(pf*2) : pf < 20 ? -(20-pf)*2 : 0;
        viking(cx, cy+bounce);
        _eyes(l, cx, cy+bounce, true, false, false, C(CLR_RED));
        FR(l, cx+28, cy-38+bounce, 4, 30, C(CLR_WOOD));
        FT(l, cx+28, cy-38+bounce, cx+44, cy-46+bounce, cx+44, cy-36+bounce, C(CLR_SILVER));
        FT(l, cx+28, cy-38+bounce, cx+32, cy-28+bounce, cx+44, cy-36+bounce, C(CLR_SILVER));
        FR(l, cx-10, cy-52+bounce, 20, 12, C(CLR_YELLOW));
        DL(l, cx-8, cy-46+bounce, cx-4, cy-44+bounce, C(CLR_BLACK));
        DL(l, cx-4, cy-44+bounce, cx+8, cy-50+bounce, C(CLR_BLACK));
        if (pf > 10) TXT(l, cx-14, cy-62+bounce, 40, C(CLR_YELLOW), "+1 HS");
    }
}

// ─── HOMELAB ──────────────────────────────────────────────────────────────────
void MascotLVGL::_drawHomelab(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    int cycle = frame % 140;

    // Desk always present
    FR(l, cx-32, cy+22, 64, 5, C(CLR_CHROME));
    DL(l, cx-32, cy+22, cx+32, cy+22, C(CLR_DIM));

    if (cycle < 80) {
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _glasses(l, cx, cy-4);
        FC(l, cx-8, cy-2, 3, C(CLR_BLACK));
        FC(l, cx+8, cy-2, 3, C(CLR_BLACK));
        FR(l, cx-12, cy-6, 9, 3, C(CLR_WHITE));
        FR(l, cx+3,  cy-6, 9, 3, C(CLR_WHITE));
        _mouth(l, cx, cy, 0);
        // Monitor showing a boot report: a small bar chart building left to
        // right instead of anonymous scrolling lines. The BOOT SUMMARY page is
        // a report card, so the screen he's reading should look like one.
        FR(l, cx-16, cy-2, 20, 16, C(CLR_CHROME));
        FR(l, cx-14, cy,   16, 12, C(CLR_NAVY));
        FR(l, cx-4,  cy+14, 8,  8, C(CLR_CHROME));
        for (int i = 0; i < 5; ++i) {
            const int grown = ((frame / 4) % 7);      // bars fill, then reset
            const int h = (i < grown) ? (2 + ((i * 3 + 2) % 8)) : 0;
            if (h > 0) {
                const lv_color_t bc = (h >= 7) ? C(CLR_GREEN)
                                    : (h >= 4) ? C(CLR_DIMCYAN) : C(CLR_YELLOW);
                FR(l, cx-13 + i*3, cy+10 - h, 2, h, bc);
            }
        }
        DL(l, cx-13, cy+11, cx-1, cy+11, C(CLR_DIM));   // chart baseline
        int tap = frame % 8 < 4 ? 1 : 0;
        FE(l, cx-28, cy+18+tap, 8, 5, C(CLR_WHITE));
        FE(l, cx+14, cy+18+tap, 8, 5, C(CLR_WHITE));
        // Coffee
        FR(l, cx+12, cy+10, 9, 10, C(CLR_CHROME));
        if (frame % 8 < 4) {
            DL(l, cx+15, cy+8, cx+14, cy+4, C(CLR_DIM));
            DL(l, cx+18, cy+8, cx+19, cy+4, C(CLR_DIM));
        }

    } else if (cycle < 110) {
        int lift = LV_MIN((int)((cycle-80) * 0.8f), 18);
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _glasses(l, cx, cy-4);
        _eyes(l, cx, cy, false, false, true, C(CLR_BLACK));
        FE(l, cx, cy+8, 4, 3, C(CLR_DIM));
        FE(l, cx+24, cy+10-lift, 8, 5, C(CLR_WHITE));
        FR(l, cx+20, cy+4-lift, 9, 10, C(CLR_CHROME));
        if (cycle > 100) DL(l, cx+23, cy+2-lift, cx+22, cy-2-lift, C(CLR_DIM));

    } else {
        int pf  = cycle - 110;
        int pop = pf < 6 ? pf*2 : pf < 10 ? 12-(pf-6) : 6;
        _ghostBody(l, cx, cy-pop, C(CLR_WHITE));
        _glasses(l, cx, cy-4-pop);
        _eyes(l, cx, cy-pop, true, false, false, C(CLR_BLACK));
        FE(l, cx, cy+8-pop, 7, 6, C(CLR_BLACK));
        FR(l, cx-5, cy+5-pop, 4, 4, C(CLR_WHITE));
        FR(l, cx+1, cy+5-pop, 4, 4, C(CLR_WHITE));
        // Lightbulb
        if (pf > 6) {
            // Bulb pulled in from cx+32 r7 with 12px rays (reached canvas x=80)
            // to cx+20 r6 with 8px rays, so the glow stops at x=64.
            int bx = cx+20, by = cy-32-pop;
            DC(l, bx, by, 6, C(CLR_YELLOW));
            DL(l, bx-3, by+6, bx+3, by+6, C(CLR_YELLOW));
            DL(l, bx-2, by+8, bx+2, by+8, C(CLR_YELLOW));
            DL(l, bx,   by-8, bx,   by-11, C(CLR_YELLOW));
            DL(l, bx+7, by-5, bx+9,  by-7, C(CLR_YELLOW));
            DL(l, bx-7, by-5, bx-9,  by-7, C(CLR_YELLOW));
            DL(l, bx+8, by+1, bx+10, by+1, C(CLR_YELLOW));
            DL(l, bx-8, by+1, bx-10, by+1, C(CLR_YELLOW));
        }
    }
}

// ─── ALERT ────────────────────────────────────────────────────────────────────
void MascotLVGL::_drawAlert(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    static const uint16_t PHASE_DUR[] = { 15, 25, 35 };
    int phase = _advancePhase(MASCOT_ALERT, PHASE_DUR, 3);
    int pf = _animPhaseFr[MASCOT_ALERT];

    if (phase == 0) {
        int shift = frame % 16 < 8 ? -2 : 2;
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), shift);
        _mouth(l, cx, cy, 0);
        _wifiRings(l, cx, cy, frame);

    } else if (phase == 1) {
        int jump = -(pf * 2);
        lv_color_t col = pf % 2 ? C(CLR_WHITE) : C(0xFFFF80);
        _ghostBody(l, cx, cy+jump, col);
        _eyes(l, cx, cy+jump, true, false, false, C(CLR_BLACK));
        _mouth(l, cx, cy+jump, 3);
        FE(l, cx-26, cy-6+jump, 7, 5, C(CLR_WHITE));
        FE(l, cx+26, cy-6+jump, 7, 5, C(CLR_WHITE));

    } else if (phase == 2) {
        int py = (cy-50) + pf*2;
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, true, false, false, C(CLR_BLACK));
        _mouth(l, cx, cy, 3);
        FE(l, cx-26, cy-6, 7, 5, C(CLR_WHITE));
        FE(l, cx+26, cy-6, 7, 5, C(CLR_WHITE));
        FR(l, cx-10, py-6, 20, 12, C(CLR_YELLOW));
        DL(l, cx-8, py, cx-4, py+2, C(CLR_BLACK));
        DL(l, cx-4, py+2, cx+8, py-4, C(CLR_BLACK));

    } else {
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, true, false, false, C(CLR_BLACK));
        _mouth(l, cx, cy, 1);
        FE(l, cx+28, cy-8, 7, 5, C(CLR_WHITE));
        FR(l, cx+18, cy-22, 20, 12, C(CLR_YELLOW));
        DL(l, cx+20, cy-16, cx+24, cy-14, C(CLR_BLACK));
        DL(l, cx+24, cy-14, cx+36, cy-20, C(CLR_BLACK));
        if (pf > 8) TXT(l, cx-18, cy-40, 50, C(CLR_GREEN), "CAUGHT");
    }
}

// ─── BAD USB ──────────────────────────────────────────────────────────────────
void MascotLVGL::_drawBadUSB(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    static const uint16_t PHASE_DUR[] = { 20, 30, 30 };
    int phase = _advancePhase(MASCOT_BAD_USB, PHASE_DUR, 3);
    int pf = _animPhaseFr[MASCOT_BAD_USB];

    // Wall socket, pulled in to cx+26..cx+33 (canvas 62..69) — it used to sit
    // at cx+38..cx+46, entirely past the panel edge.
    FR(l, cx+26, cy-10, 7, 24, C(CLR_CHROME));
    FR(l, cx+28, cy-2,  2,  5, C(CLR_BLACK));
    FR(l, cx+31, cy-2,  2,  5, C(CLR_BLACK));

    if (phase == 0) {
        int shift = frame % 42 < 14 ? -3 : frame % 42 < 28 ? 3 : 0;
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), shift);
        _mouth(l, cx, cy, 0);
        FR(l, cx+6,  cy+2, 12, 7, C(CLR_DIM));
        FR(l, cx+12, cy+3,  6, 5, C(CLR_CHROME));
        FR(l, cx+14, cy+4,  2, 3, C(CLR_BLACK));
        FR(l, cx+17, cy+4,  2, 3, C(CLR_BLACK));

    } else if (phase == 1) {
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), 3);
        _mouth(l, cx, cy, 0);
        // Stick slides right until its tip meets the socket at cx+26.
        int usbX = LV_MIN(cx+6 + (int)(pf*1.2f), cx+14);
        FR(l, usbX, cy+2, 12, 7, C(CLR_DIM));
        FR(l, usbX+6, cy+3, 6, 5, C(CLR_CHROME));
        FR(l, usbX+8, cy+4, 2, 3, C(CLR_BLACK));
        FR(l, usbX+11,cy+4, 2, 3, C(CLR_BLACK));

    } else if (phase == 2) {
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, true, false, C(CLR_BLACK), 3);
        _mouth(l, cx, cy, 1);
        FR(l, cx+14, cy+2, 12, 7, C(CLR_DIM));   // seated in the socket
        _sparks(l, cx, cy, frame);
        if (pf % 4 < 2) DC(l, cx, cy, 28, C(CLR_RED));

    } else {
        int bounce = pf < 8 ? -(pf*2) : pf < 16 ? -(16-pf)*2 : 0;
        _ghostBody(l, cx, cy+bounce, C(CLR_WHITE));
        _eyes(l, cx, cy+bounce, true, false, false, C(CLR_BLACK));
        _mouth(l, cx, cy+bounce, 1);
        FE(l, cx-26, cy-6+bounce, 7, 5, C(CLR_WHITE));
        FE(l, cx+26, cy-6+bounce, 7, 5, C(CLR_WHITE));
        if (pf > 10) {
            lv_color_t col = pf % 4 < 2 ? C(CLR_YELLOW) : C(CLR_GREEN);
            TXT(l, cx-22, cy-46+bounce, 60, col, "INJECTED");
        }
    }
}

// ─── TRANSMIT ─────────────────────────────────────────────────────────────────
// Speaking out over the link: mouth open, hands raised, plus a live TX level
// meter on the left so the state reads as "actively transmitting" rather than
// just "ghost with arcs next to it".
void MascotLVGL::_drawTransmit(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    const int bob = _bob();
    cy += bob;
    _groundShadow(l, cx, bob);
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 3);
    FE(l, cx-22, cy+4, 7, 5, C(CLR_WHITE));
    FE(l, cx+22, cy+4, 7, 5, C(CLR_WHITE));
    _soundWaves(l, cx, cy, frame);

    // TX level meter: five segments driven by a bouncing level.
    const int lvl = 1 + ((frame / 2) % 5);
    for (int i = 0; i < 5; ++i) {
        const lv_color_t sc = (i >= lvl)      ? C(0x1A1A1A)
                            : (i >= 4)        ? C(CLR_RED)
                            : (i >= 3)        ? C(CLR_YELLOW)
                                              : C(CLR_GREEN);
        FR(l, cx - 30, cy + 12 - i * 5, 7, 4, sc);
    }
}

// ─── LOW BATTERY ──────────────────────────────────────────────────────────────
void MascotLVGL::_drawLowBattery(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _groundShadow(l, cx, 0);
    lv_color_t bc = frame % 6 < 1 ? C(CLR_DIM) : C(CLR_WHITE);
    _ghostBody(l, cx, cy, bc);
    _eyes(l, cx, cy, false, false, true, C(CLR_DIM));
    _mouth(l, cx, cy, 2);
    FC(l, cx+20, cy-10, 4, C(CLR_CYAN));
    FT(l, cx+20, cy-18, cx+16, cy-10, cx+24, cy-10, C(CLR_CYAN));
    // Battery icon
    {
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_RED); rd.border_width = 1; rd.radius = 0;
        lv_area_t a = {(int32_t)(cx-10),(int32_t)(cy-45),(int32_t)(cx+9),(int32_t)(cy-36)};
        lv_draw_rect(l, &rd, &a);
    }
    FR(l, cx+10, cy-43, 3, 6, C(CLR_RED));
    FR(l, cx-8,  cy-43, 4, 6, C(CLR_RED));
}

// ─── ERROR ────────────────────────────────────────────────────────────────────
void MascotLVGL::_drawError(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    _ghostBody(l, cx, cy, C(0xF00010));
    DL(l, cx-12, cy-8, cx-5,  cy-1, C(CLR_RED), 2);
    DL(l, cx-5,  cy-8, cx-12, cy-1, C(CLR_RED), 2);
    DL(l, cx+5,  cy-8, cx+12, cy-1, C(CLR_RED), 2);
    DL(l, cx+12, cy-8, cx+5,  cy-1, C(CLR_RED), 2);
    _mouth(l, cx, cy, 2);
    FE(l, cx+8, cy-12, 18, 7, C(CLR_WHITE));
    FR(l, cx-30, cy-44, 6, 16, C(CLR_RED));
    FC(l, cx-27, cy-24, 3, C(CLR_RED));
}

// ─── MESHTASTIC ───────────────────────────────────────────────────────────────
// Matches the MESHTASTIC page (node id, channel, RX/TX, last message): the
// ghost is a relay operator holding a node radio, with peer nodes blinking in
// and out around it and a chat bubble when traffic lands.
void MascotLVGL::_drawMeshtastic(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    const int bob = _bob();
    cy += bob;

    _groundShadow(l, cx, bob);

    // Peer mesh nodes orbiting the ghost's head, each on its own phase so the
    // mesh looks like it is discovering/losing neighbours.
    const int nodeX[3] = {cx-26, cx+26, cx-2};
    const int nodeY[3] = {cy-30, cy-24, cy-40};
    for (int i = 0; i < 3; ++i) {
        const bool live = ((frame / 6) + i) % 3 != 0;
        const lv_color_t nc = live ? C(CLR_GREEN) : C(CLR_DIMCYAN);
        // Link line back to the node radio, drawn first so nodes sit on top.
        DL(l, cx+12, cy-8, nodeX[i], nodeY[i], live ? C(CLR_DIMCYAN) : C(0x123033));
        FC(l, nodeX[i], nodeY[i], live ? 3 : 2, nc);
    }

    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);

    // Handheld node radio with a stubby antenna.
    FR(l, cx+10, cy-6, 8, 14, C(CLR_CHROME));
    FR(l, cx+12, cy-4, 4,  4, C(CLR_GREEN));
    DL(l, cx+16, cy-6, cx+19, cy-16, C(CLR_DIM));
    FC(l, cx+19, cy-17, 2, C(CLR_GREEN));
    FE(l, cx+8, cy+6, 6, 4, C(CLR_WHITE));   // hand

    // Incoming message bubble on a slow cycle — the page's "last message" row.
    if ((frame / 10) % 4 == 3) {
        FR(l, cx-30, cy-18, 18, 11, C(CLR_CYAN));
        FT(l, cx-24, cy-7, cx-20, cy-7, cx-22, cy-3, C(CLR_CYAN));
        FC(l, cx-25, cy-13, 1, C(CLR_BLACK));
        FC(l, cx-21, cy-13, 1, C(CLR_BLACK));
        FC(l, cx-17, cy-13, 1, C(CLR_BLACK));
    }
}

// ─── UPLINK ───────────────────────────────────────────────────────────────────
// Matches the uplink mission (sync pipeline, publish progress): records lift
// off the ghost's hands into a cloud, with a fill bar that tracks the climb.
void MascotLVGL::_drawUplink(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    const int bob = _bob();
    cy += bob;

    _groundShadow(l, cx, bob);
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), -1);
    _mouth(l, cx, cy, 0);

    // Cloud endpoint above.
    const int clx = cx + 6, cly = cy - 36;
    FE(l, clx,    cly,    11, 6, C(CLR_DIMCYAN));
    FE(l, clx-7,  cly+2,  6,  4, C(CLR_DIMCYAN));
    FE(l, clx+7,  cly+2,  6,  4, C(CLR_DIMCYAN));

    // Three packets climbing on staggered phases.
    for (int i = 0; i < 3; ++i) {
        const int ph = (frame + i * 5) % 15;
        const int py = cy - 12 - ph * 2;
        if (py < cly + 4) continue;
        const lv_color_t pc = (ph > 10) ? C(CLR_DIMCYAN) : C(CLR_GREEN);
        FR(l, cx + 2 + (i - 1) * 7, py, 5, 4, pc);
    }

    // Raised hands, sending.
    FE(l, cx-17, cy-6, 6, 4, C(CLR_WHITE));
    FE(l, cx+17, cy-6, 6, 4, C(CLR_WHITE));

    // Progress bar under the ghost — mirrors the page's publish percentage.
    const int barW = 34;
    const int fill = 4 + ((frame / 2) % (barW - 6));
    FR(l, cx - barW/2, cy + 40, barW, 5, C(0x1A1A1A));
    FR(l, cx - barW/2 + 1, cy + 41, fill, 3, C(CLR_GREEN));
}

// ─── PREFLIGHT ────────────────────────────────────────────────────────────────
void MascotLVGL::_drawPreflight(lv_layer_t* l, int frame) {
    int cx = MASCOT_CX, cy = MASCOT_CY;
    const int bob = _bob();
    cy += bob;
    _groundShadow(l, cx, bob);
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);
    FR(l, cx+8,  cy-12, 22, 28, C(CLR_CHROME));
    FR(l, cx+14, cy-15, 10,  6, C(CLR_DIM));
    DL(l, cx+16, cy-4, cx+26, cy-4, C(CLR_DIM));
    DL(l, cx+16, cy+2, cx+26, cy+2, C(CLR_DIM));
    DL(l, cx+16, cy+8, cx+26, cy+8, C(CLR_DIM));
    int checks = (frame / 25) % 4;
    if (checks > 0) { DL(l, cx+10,cy-6,cx+13,cy-3,C(CLR_GREEN)); DL(l, cx+13,cy-3,cx+16,cy-8,C(CLR_GREEN)); }
    if (checks > 1) { DL(l, cx+10,cy,  cx+13,cy+3,C(CLR_GREEN)); DL(l, cx+13,cy+3,cx+16,cy-2,C(CLR_GREEN)); }
    if (checks > 2) { DL(l, cx+10,cy+6,cx+13,cy+9,C(CLR_YELLOW)); DL(l, cx+13,cy+9,cx+16,cy+4,C(CLR_YELLOW)); }
    DL(l, cx+6, cy+12, cx+12, cy+5, C(CLR_YELLOW), 2);
    FC(l, cx+6, cy+13, 2, C(CLR_YELLOW));

    // Launch-readiness lamps on the left, ticking green in step with the
    // checklist. Same pre-flight theme, but the state now shows a GO condition
    // building instead of only a pen moving — which is what MISSION LAUNCH is
    // actually asking the operator to confirm.
    const bool allGo = checks >= 3;
    for (int i = 0; i < 3; ++i) {
        const bool on = checks > i;
        FC(l, cx-27, cy-9 + i*9, 3,
           on ? (allGo ? C(CLR_GREEN) : C(CLR_YELLOW)) : C(0x1A1A1A));
        DC(l, cx-27, cy-9 + i*9, 4, C(CLR_DIM));
    }
    // Armed strip pulses once every lamp is lit.
    if (allGo && (frame / 3) % 2) {
        FR(l, cx-33, cy+22, 16, 4, C(CLR_GREEN));
    }
}


