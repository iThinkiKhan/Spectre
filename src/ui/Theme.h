


#pragma once
#include "lvgl.h"

// ─── Font declarations ────────────────────────────────────────
LV_FONT_DECLARE(orbitron_bold_uncompressed_32);
LV_FONT_DECLARE(orbitron_bold_uncompressed_16);
LV_FONT_DECLARE(spacemono_bold_14);
LV_FONT_DECLARE(sharetech_uncompressed_14);

// ─── Font references (use these everywhere) ───────────────────
#define FONT_TITLE      (&orbitron_bold_uncompressed_32)
#define FONT_HEADER     (&orbitron_bold_uncompressed_16)
#define FONT_BODY       (&spacemono_bold_14)
#define FONT_SMALL      (&sharetech_uncompressed_14)

// ─── CP2077 Color palette ─────────────────────────────────────
#define CLR_YELLOW      0xFCE700
#define CLR_CYAN        0x00F0FF
#define CLR_RED         0xFF003C
#define CLR_GREEN       0x00FF9C
#define CLR_HOTPINK     0xFF00FF
#define CLR_WHITE       0xFFFFFF
#define CLR_DIM         0x7BEF
#define CLR_DIMYELLOW   0xA07800
#define CLR_DIMCYAN     0x007880
#define CLR_GREY        0x8A8A8A
#define CLR_DARKGREY    0x2A2A2A
#define CLR_CHROME      0x2104
#define CLR_NAVY        0x000F
#define CLR_ORANGE      0xFD20
#define CLR_BLACK       0x000000
#define CLR_DARKGREY    0x2A2A2A

// ─── LVGL color helpers ───────────────────────────────────────
#define LV_CLR(hex)     lv_color_hex(hex)

// ─── Layout constants ─────────────────────────────────────────
#define THEME_STATUS_H      24
#define THEME_ACTION_H      18
#define THEME_MASCOT_W      72
#define THEME_SCREEN_W      320
#define THEME_SCREEN_H      170
#define THEME_CONTENT_X     (THEME_MASCOT_W + 4)
#define THEME_CONTENT_W     (THEME_SCREEN_W - THEME_MASCOT_W - 4)
#define THEME_CONTENT_Y     THEME_STATUS_H
#define THEME_CONTENT_H     (THEME_SCREEN_H - THEME_STATUS_H - THEME_ACTION_H)
#define THEME_ACTION_Y      (THEME_SCREEN_H - THEME_ACTION_H)
#define THEME_DIVIDER_X     THEME_MASCOT_W

// ─── Reusable style builders ──────────────────────────────────
// Call once at init, apply via lv_obj_add_style()

static inline void theme_style_panel(lv_style_t* s, uint32_t bg, uint32_t border) {
    lv_style_init(s);
    lv_style_set_bg_color(s, lv_color_hex(bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, lv_color_hex(border));
    lv_style_set_border_width(s, 1);
    lv_style_set_radius(s, 0);
    lv_style_set_pad_all(s, 0);
}

static inline void theme_style_label(lv_style_t* s, uint32_t color, const lv_font_t* font) {
    lv_style_init(s);
    lv_style_set_text_color(s, lv_color_hex(color));
    lv_style_set_text_font(s, font);
    lv_style_set_bg_opa(s, LV_OPA_TRANSP);
}




