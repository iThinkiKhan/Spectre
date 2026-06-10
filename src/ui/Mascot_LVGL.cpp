
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

void MascotLVGL::_ghostBody(lv_layer_t* l, int cx, int cy, lv_color_t color) {
    FE(l, cx, cy, 22, 24, color);
    FR(l, cx-22, cy, 44, 18, color);
    for (int bx : {cx-14, cx, cx+14}) FC(l, bx, cy+18, 9, color);
    FR(l, cx-22, cy+22, 44, 8, C(CLR_BLACK));
    FC(l, cx-7, cy+24, 5, C(CLR_BLACK));
    FC(l, cx+7, cy+24, 5, C(CLR_BLACK));
    lv_color_t outline = C(CLR_DIM);
    DE(l, cx, cy, 22, 24, outline);
}

void MascotLVGL::_eyes(lv_layer_t* l, int cx, int cy,
                   bool wide, bool angry, bool halfClosed,
                   lv_color_t color, int shift) {
    int r = wide ? 4 : 3;
    FC(l, cx-8+shift, cy-4, r, color);
    FC(l, cx+8+shift, cy-4, r, color);
    if (halfClosed) {
        FR(l, cx-13, cy-8, 11, 4, C(CLR_WHITE));
        FR(l, cx+2,  cy-8, 11, 4, C(CLR_WHITE));
    }
    if (angry) {
        DL(l, cx-13, cy-9, cx-4, cy-7, C(CLR_BLACK));
        DL(l, cx+4,  cy-7, cx+13, cy-9, C(CLR_BLACK));
    }
}

void MascotLVGL::_mouth(lv_layer_t* l, int cx, int cy, int type) {
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

void MascotLVGL::_satDish(lv_layer_t* l, int cx, int cy, int frame) {
    int dx = cx+32, dy = cy+8;
    DL(l, dx, dy, dx, dy-16, C(CLR_DIM));
    FE(l, dx, dy-18, 10, 5, C(CLR_DIM));
    DL(l, dx, dy-23, dx-8, dy-30, C(CLR_DIM));
    int ph = frame % 8;
    if (ph > 0) DC(l, dx-10, dy-32, 4,  C(CLR_YELLOW));
    if (ph > 2) DC(l, dx-14, dy-36, 8,  C(CLR_DIMYELLOW));
    if (ph > 4) DC(l, dx-18, dy-40, 12, C(CLR_CHROME));
}

void MascotLVGL::_wifiRings(lv_layer_t* l, int cx, int cy, int frame) {
    FR(l, cx+18, cy-14, 10, 14, C(CLR_CHROME));
    {   // scanner device outline
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_CYAN); rd.border_width = 1; rd.radius = 0;
        lv_area_t a = {(int32_t)(cx+18),(int32_t)(cy-14),(int32_t)(cx+27),(int32_t)(cy)};
        lv_draw_rect(l, &rd, &a);
    }
    FR(l, cx+20, cy-12, 6, 4, C(CLR_NAVY));
    FE(l, cx+14, cy+2, 7, 4, C(CLR_WHITE));
    int ph = (frame % 8) + 1;
    if (ph > 1) DC(l, cx+38, cy-8, 5,  C(CLR_CYAN));
    if (ph > 3) DC(l, cx+38, cy-8, 10, C(CLR_DIMCYAN));
    if (ph > 5) DC(l, cx+38, cy-8, 15, C(CLR_CHROME));
}

void MascotLVGL::_soundWaves(lv_layer_t* l, int cx, int cy, int frame) {
    int ph = frame % 6;
    DA(l, cx+30, cy, 8,  315, 45, ph > 0 ? C(CLR_YELLOW) : C(CLR_DIMYELLOW));
    DA(l, cx+30, cy, 14, 315, 45, ph > 2 ? C(CLR_YELLOW) : C(CLR_DIMYELLOW));
    DA(l, cx+30, cy, 20, 315, 45, ph > 4 ? C(CLR_DIMYELLOW) : C(CLR_DIM));
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
    int ph = frame % 12;
    const uint32_t cols[] = { CLR_YELLOW, CLR_DIMYELLOW, CLR_DIM, 0x202020 };
    for (int i = 0; i < 4; i++) {
        int dx = cx - 30 - (i * 9) - (ph < 6 ? 1 : 0);
        FC(l, dx, cy+14, i < 2 ? 2 : 1, C(cols[i]));
    }
}

void MascotLVGL::_glasses(lv_layer_t* l, int cx, int cy) {
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

void MascotLVGL::_vikingHorn(lv_layer_t* l, int cx, int cy, int dir) {
    lv_color_t bone = C(CLR_BONE), shadow = C(CLR_BONE_SHD);
    int bx = cx + dir*14;
    FT(l, bx,         cy-16, bx,            cy-22, cx+dir*28, cy-26, bone);
    FT(l, bx,         cy-22, cx+dir*28,     cy-26, cx+dir*36, cy-36, bone);
    FT(l, cx+dir*28,  cy-26, cx+dir*36,     cy-36, cx+dir*42, cy-44, bone);
    FT(l, cx+dir*36,  cy-36, cx+dir*42,     cy-44, cx+dir*44, cy-50, bone);
    DL(l, bx+dir*2, cy-18, cx+dir*30, cy-26, shadow);
    DL(l, cx+dir*30, cy-26, cx+dir*40, cy-40, shadow);
    DL(l, cx+dir*40, cy-40, cx+dir*44, cy-50, shadow);
}

void MascotLVGL::_vikingHelmet(lv_layer_t* l, int cx, int cy) {
    FE(l, cx, cy-26, 24, 12, C(CLR_STEEL));
    FR(l, cx-24, cy-28, 48, 8,  C(CLR_STEEL));
    FE(l, cx, cy-30, 18, 8,  C(CLR_STEEL_DK));
    FR(l, cx-3, cy-26, 6, 12, C(CLR_NOSEGUARD));
    DL(l, cx-24, cy-22, cx+24, cy-22, C(CLR_BONE));  // brim highlight
}

void MascotLVGL::_furCloak(lv_layer_t* l, int cx, int cy) {
    lv_color_t fur = C(CLR_FUR), furDk = C(CLR_FUR_DK);
    FT(l, cx-28, cy+8, cx-36, cy+42, cx+36, cy+42, fur);
    FT(l, cx-28, cy+8, cx+28, cy+8,  cx+36, cy+42, fur);
    for (int i = 0; i < 6; i++) FC(l, cx-22 + i*9, cy+42, 4, furDk);
}

void MascotLVGL::_vikingBeard(lv_layer_t* l, int cx, int cy) {
    lv_color_t beard = C(CLR_BEARD), wood = C(CLR_WOOD);
    FR(l, cx-14, cy+6, 28, 18, beard);
    FT(l, cx-14, cy+24, cx+14, cy+24, cx, cy+36, beard);
    DL(l, cx-4, cy+8, cx-6, cy+30, wood);
    DL(l, cx+4, cy+8, cx+6, cy+30, wood);
}

void MascotLVGL::_battleAxe(lv_layer_t* l, int cx, int cy, int yOff) {
    FR(l, cx+28, cy-8+yOff, 4, 30, C(CLR_WOOD));
    FT(l, cx+28, cy-8+yOff, cx+44, cy-14+yOff, cx+44, cy-4+yOff,  C(CLR_SILVER));
    FT(l, cx+28, cy-8+yOff, cx+32, cy+2+yOff,  cx+44, cy-4+yOff,  C(CLR_SILVER));
    DL(l, cx+44, cy-14+yOff, cx+44, cy-4+yOff, C(CLR_STEEL));
    DL(l, cx+28, cy-8+yOff,  cx+44, cy-14+yOff, C(CLR_STEEL_DK));
}

void MascotLVGL::_roundShield(lv_layer_t* l, int cx, int cy) {
    FC(l, cx-30, cy+4, 12, C(CLR_FUR));
    DC(l, cx-30, cy+4, 12, C(CLR_WOOD), 2);
    DL(l, cx-30, cy-8,  cx-30, cy+16, C(CLR_BEARD));
    DL(l, cx-42, cy+4,  cx-18, cy+4,  C(CLR_BEARD));
    FC(l, cx-30, cy+4, 3, C(CLR_BEARD));
}

void MascotLVGL::_mouthFangs(lv_layer_t* l, int cx, int cy) {
    FR(l, cx-9, cy+5, 18, 8, C(CLR_BLACK));
    FT(l, cx-8, cy+5, cx-4, cy+13, cx,   cy+5, C(CLR_WHITE));
    FT(l, cx+1, cy+5, cx+5, cy+13, cx+9, cy+5, C(CLR_WHITE));
    FT(l, cx-3, cy+5, cx-1, cy+8,  cx+1, cy+5, C(CLR_DIM));
}

void MascotLVGL::_satDishLeft(lv_layer_t* l, int cx, int cy, int frame) {
    int dx = cx-42, dy = cy+10;
    DL(l, dx, dy, dx, dy-18, C(CLR_DIM));
    FE(l, dx, dy-20, 12, 6, C(CLR_DARKGREY));
    DC(l, dx, dy-20, 12, C(CLR_YELLOW));
    DL(l, dx, dy-26, dx-6, dy-32, C(CLR_DIM));
    int ph = frame % 10;
    if (ph > 2) DA(l, cx-20, cy-8, 8,  150, 210, C(CLR_YELLOW));
    if (ph > 5) DA(l, cx-20, cy-8, 14, 150, 210, C(CLR_DIMYELLOW));
    if (ph > 8) DA(l, cx-20, cy-8, 20, 150, 210, C(CLR_DARKGREY));
}

void MascotLVGL::_notepad(lv_layer_t* l, int cx, int cy, int frame) {
    FR(l, cx+16, cy-2, 18, 22, C(CLR_BONE));
    {
        lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
        rd.bg_opa = LV_OPA_TRANSP; rd.border_color = C(CLR_BONE_SHD); rd.border_width = 1; rd.radius = 0;
        lv_area_t a = {(int32_t)(cx+16),(int32_t)(cy-2),(int32_t)(cx+33),(int32_t)(cy+19)};
        lv_draw_rect(l, &rd, &a);
    }
    for (int row = 0; row < 4; row++)
        DL(l, cx+18, cy+3+row*5, cx+32, cy+3+row*5, C(CLR_BONE_SHD));
    int lineRow = (frame / 16) % 4;
    int lineLen = LV_MIN(frame % 16, 14);
    DL(l, cx+18, cy+3+lineRow*5, cx+18+lineLen, cy+3+lineRow*5, C(CLR_BLACK));
    int tap = (frame % 8 < 4) ? 1 : 0;
    FE(l, cx+28, cy+14+tap, 5, 4, C(CLR_WHITE));
    DL(l, cx+30, cy+10+tap, cx+36, cy+4+tap, C(CLR_YELLOW), 2);
    FC(l, cx+30, cy+10+tap, 2, C(CLR_YELLOW));
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
    int cx = 45, cy = 58;
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _helmet(l, cx, cy);
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);
    if (frame < 10) {
        int armX = cx+14 + frame*2/3;
        int armY = cy+2  - frame*2;
        FE(l, armX, armY, 6, 5, C(CLR_WHITE));
    } else {
        FE(l, cx+22, cy-16, 6, 5, C(CLR_WHITE));
        DL(l, cx+14, cy+2, cx+20, cy-14, C(CLR_WHITE), 2);
    }
}

// ─── STANDBY ──────────────────────────────────────────────────────────────────
void MascotLVGL::_drawStandby(lv_layer_t* l, int frame) {
    int cx = 45, cy = 58;
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
}

// ─── RECON (formerly ReconWalk) ───────────────────────────────────────────────
void MascotLVGL::_drawReconWalk(lv_layer_t* l, int frame) {
    int cx = 45, cy = 58;
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
void MascotLVGL::_drawWifiRecon(lv_layer_t* l, int frame) {
    int cx = 40, cy = 58;
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    DL(l, cx-12, cy-9, cx-5, cy-7, C(CLR_BLACK));
    DL(l, cx+5,  cy-7, cx+12, cy-9, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);
    _wifiRings(l, cx, cy, frame);
}

// ─── LORA RECON ───────────────────────────────────────────────────────────────
void MascotLVGL::_drawLoraRecon(lv_layer_t* l, int frame) {
    int cx = 42, cy = 58;
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    FE(l, cx+26, cy-2, 9, 14, C(CLR_WHITE));
    FE(l, cx+26, cy-2, 5,  9, C(CLR_DIM));
    FE(l, cx+26, cy-2, 2,  4, C(CLR_BLACK));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 0);
    int ph = frame % 8;
    if (ph > 0) DL(l, cx+40, cy-2, cx+37, cy-2, C(CLR_YELLOW));
    if (ph > 2) DA(l, cx+44, cy-2, 6,  160, 200, C(CLR_YELLOW));
    if (ph > 4) DA(l, cx+44, cy-2, 12, 160, 200, C(CLR_DIMYELLOW));
    if (ph > 6) DA(l, cx+44, cy-2, 18, 160, 200, C(CLR_DIM));
}

// ─── PWNAGOTCHI — Full Viking ─────────────────────────────────────────────────
void MascotLVGL::_drawPwny(lv_layer_t* l, int frame) {
    int cx = 45, cy = 60;
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
    int cx = 44, cy = 62;
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
        // Monitor
        FR(l, cx-16, cy-2, 20, 16, C(CLR_CHROME));
        FR(l, cx-14, cy,   16, 12, C(CLR_NAVY));
        FR(l, cx-4,  cy+14, 8,  8, C(CLR_CHROME));
        int scroll = (frame/3) % 4;
        DL(l, cx-12, cy+2+scroll,   cx-4, cy+2+scroll,   C(CLR_GREEN));
        DL(l, cx-12, cy+5+scroll,   cx-8, cy+5+scroll,   C(CLR_DIMCYAN));
        DL(l, cx-12, cy+4+scroll-3, cx-6, cy+4+scroll-3, C(CLR_YELLOW));
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
            int bx = cx+32, by = cy-34-pop;
            DC(l, bx, by, 7, C(CLR_YELLOW));
            DL(l, bx-4, by+7, bx+4, by+7, C(CLR_YELLOW));
            DL(l, bx-3, by+10, bx+3, by+10, C(CLR_YELLOW));
            DL(l, bx,   by-10, bx,   by-13, C(CLR_YELLOW));
            DL(l, bx+8, by-6,  bx+11, by-9, C(CLR_YELLOW));
            DL(l, bx-8, by-6,  bx-11, by-9, C(CLR_YELLOW));
            DL(l, bx+9, by+2,  bx+12, by+2, C(CLR_YELLOW));
            DL(l, bx-9, by+2,  bx-12, by+2, C(CLR_YELLOW));
        }
    }
}

// ─── ALERT ────────────────────────────────────────────────────────────────────
void MascotLVGL::_drawAlert(lv_layer_t* l, int frame) {
    int cx = 45, cy = 60;
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
    int cx = 44, cy = 58;
    static const uint16_t PHASE_DUR[] = { 20, 30, 30 };
    int phase = _advancePhase(MASCOT_BAD_USB, PHASE_DUR, 3);
    int pf = _animPhaseFr[MASCOT_BAD_USB];

    // Wall socket always visible
    FR(l, cx+38, cy-10, 8, 26, C(CLR_CHROME));
    FR(l, cx+40, cy-2,  2,  5, C(CLR_BLACK));
    FR(l, cx+43, cy-2,  2,  5, C(CLR_BLACK));

    if (phase == 0) {
        int shift = frame % 42 < 14 ? -3 : frame % 42 < 28 ? 3 : 0;
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), shift);
        _mouth(l, cx, cy, 0);
        FR(l, cx+14, cy+2, 12, 7, C(CLR_DIM));
        FR(l, cx+20, cy+3,  6, 5, C(CLR_CHROME));
        FR(l, cx+22, cy+4,  2, 3, C(CLR_BLACK));
        FR(l, cx+25, cy+4,  2, 3, C(CLR_BLACK));

    } else if (phase == 1) {
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, false, false, C(CLR_BLACK), 3);
        _mouth(l, cx, cy, 0);
        int usbX = LV_MIN(cx+14 + (int)(pf*1.2f), cx+26);
        FR(l, usbX, cy+2, 12, 7, C(CLR_DIM));
        FR(l, usbX+6, cy+3, 6, 5, C(CLR_CHROME));
        FR(l, usbX+8, cy+4, 2, 3, C(CLR_BLACK));
        FR(l, usbX+11,cy+4, 2, 3, C(CLR_BLACK));

    } else if (phase == 2) {
        _ghostBody(l, cx, cy, C(CLR_WHITE));
        _eyes(l, cx, cy, false, true, false, C(CLR_BLACK), 3);
        _mouth(l, cx, cy, 1);
        FR(l, cx+26, cy+2, 12, 7, C(CLR_DIM));
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
void MascotLVGL::_drawTransmit(lv_layer_t* l, int frame) {
    int cx = 42, cy = 58;
    _ghostBody(l, cx, cy, C(CLR_WHITE));
    _eyes(l, cx, cy, false, false, false, C(CLR_BLACK));
    _mouth(l, cx, cy, 3);
    FE(l, cx-22, cy+4, 7, 5, C(CLR_WHITE));
    FE(l, cx+22, cy+4, 7, 5, C(CLR_WHITE));
    _soundWaves(l, cx, cy, frame);
}

// ─── LOW BATTERY ──────────────────────────────────────────────────────────────
void MascotLVGL::_drawLowBattery(lv_layer_t* l, int frame) {
    int cx = 45, cy = 58;
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
    int cx = 45, cy = 58;
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

// ─── PREFLIGHT ────────────────────────────────────────────────────────────────
void MascotLVGL::_drawPreflight(lv_layer_t* l, int frame) {
    int cx = 44, cy = 58;
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
}


