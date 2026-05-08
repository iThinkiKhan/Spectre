

#include "BootSequence.h"
#include "MascotState.h"
#include <Arduino.h>

static void _typewrite(lv_obj_t* label, const char* text, int delayMs) {
    String buf = "";
    for (int i = 0; i < (int)strlen(text); i++) {
        buf += text[i];
        lv_label_set_text(label, buf.c_str());
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }
}

static void _glitchFlash(lv_obj_t* titleLabel) {
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xFF003C), 0);
    lv_obj_set_pos(titleLabel, THEME_SCREEN_W/2 - 2, 8);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(40));

    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_pos(titleLabel, THEME_SCREEN_W/2 + 2, 8);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(40));

    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xFCE700), 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 8);
    lv_refr_now(NULL);
}

void runBootSequence(DisplayManager& display, bool loraOk, bool storageOk) {

#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] start");
#endif

    // ── Phase 1: black screen + corner brackets ───────────────
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] phase1 corners");
#endif
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    static lv_point_precise_t tlH[2] = {{0,0},{0,0}};
    static lv_point_precise_t tlV[2] = {{0,0},{0,0}};
    static lv_point_precise_t trH[2] = {{THEME_SCREEN_W,0},{THEME_SCREEN_W,0}};
    static lv_point_precise_t trV[2] = {{THEME_SCREEN_W,0},{THEME_SCREEN_W,0}};
    static lv_point_precise_t blH[2] = {{0,THEME_SCREEN_H-1},{0,THEME_SCREEN_H-1}};
    static lv_point_precise_t blV[2] = {{0,THEME_SCREEN_H-1},{0,THEME_SCREEN_H-1}};
    static lv_point_precise_t brH[2] = {{THEME_SCREEN_W,THEME_SCREEN_H-1},{THEME_SCREEN_W,THEME_SCREEN_H-1}};
    static lv_point_precise_t brV[2] = {{THEME_SCREEN_W,THEME_SCREEN_H-1},{THEME_SCREEN_W,THEME_SCREEN_H-1}};

    auto makeCornerLine = [&](lv_point_precise_t* pts) -> lv_obj_t* {
        lv_obj_t* l = lv_line_create(scr);
        lv_line_set_points(l, pts, 2);
        lv_obj_set_style_line_color(l, lv_color_hex(0xFCE700), 0);
        lv_obj_set_style_line_width(l, 2, 0);
        return l;
    };

    lv_obj_t* cTLH = makeCornerLine(tlH);
    lv_obj_t* cTLV = makeCornerLine(tlV);
    lv_obj_t* cTRH = makeCornerLine(trH);
    lv_obj_t* cTRV = makeCornerLine(trV);
    lv_obj_t* cBLH = makeCornerLine(blH);
    lv_obj_t* cBLV = makeCornerLine(blV);
    lv_obj_t* cBRH = makeCornerLine(brH);
    lv_obj_t* cBRV = makeCornerLine(brV);

    for (int i = 0; i <= 28; i += 2) {
        tlH[1] = {(lv_value_precise_t)i, 0};
        tlV[1] = {0, (lv_value_precise_t)i};
        trH[1] = {(lv_value_precise_t)(THEME_SCREEN_W - i), 0};
        trV[1] = {THEME_SCREEN_W, (lv_value_precise_t)i};
        blH[1] = {(lv_value_precise_t)i, THEME_SCREEN_H-1};
        blV[1] = {0, (lv_value_precise_t)(THEME_SCREEN_H - 1 - i)};
        brH[1] = {(lv_value_precise_t)(THEME_SCREEN_W - i), THEME_SCREEN_H-1};
        brV[1] = {THEME_SCREEN_W, (lv_value_precise_t)(THEME_SCREEN_H - 1 - i)};

        lv_line_set_points(cTLH, tlH, 2);
        lv_line_set_points(cTLV, tlV, 2);
        lv_line_set_points(cTRH, trH, 2);
        lv_line_set_points(cTRV, trV, 2);
        lv_line_set_points(cBLH, blH, 2);
        lv_line_set_points(cBLV, blV, 2);
        lv_line_set_points(cBRH, brH, 2);
        lv_line_set_points(cBRV, brV, 2);

        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(12));
    }

    vTaskDelay(pdMS_TO_TICKS(150));

    // ── Phase 2: SPECTRE title with chromatic aberration ──────
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] phase2 title");
#endif
    lv_obj_t* titleLabel = lv_label_create(scr);
    lv_label_set_text(titleLabel, "SPECTRE");
    lv_obj_set_style_text_font(titleLabel, FONT_TITLE, 0);
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0xFCE700), 0);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 8);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(200));

    _glitchFlash(titleLabel);
    vTaskDelay(pdMS_TO_TICKS(80));
    _glitchFlash(titleLabel);
    vTaskDelay(pdMS_TO_TICKS(120));

    // ── Phase 3: subtitle typewriter ──────────────────────────
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] phase3 subtitle");
#endif
    lv_obj_t* subLabel = lv_label_create(scr);
    lv_label_set_text(subLabel, "");
    lv_obj_set_style_text_font(subLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(subLabel, lv_color_hex(0x00F0FF), 0);
    lv_obj_align(subLabel, LV_ALIGN_TOP_MID, 0, 52);
    lv_refr_now(NULL);

    _typewrite(subLabel, "FIELD INTELLIGENCE PLATFORM", 28);
    vTaskDelay(pdMS_TO_TICKS(100));

    // ── Phase 4: separator line draws left to right ───────────
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] phase4 separator");
#endif
    static lv_point_precise_t sepPts[2] = {{0, 72}, {0, 72}};
    lv_obj_t* sepLine = lv_line_create(scr);
    lv_line_set_points(sepLine, sepPts, 2);
    lv_obj_set_style_line_color(sepLine, lv_color_hex(0xFCE700), 0);
    lv_obj_set_style_line_width(sepLine, 2, 0);

    for (int x = 0; x <= THEME_SCREEN_W; x += 12) {
        sepPts[1].x = x;
        lv_line_set_points(sepLine, sepPts, 2);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(6));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── Phase 5: system checks cascade ───────────────────────
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] phase5 mascot area");
#endif
    lv_obj_t* mascotArea = lv_obj_create(scr);
    lv_obj_set_pos(mascotArea, THEME_SCREEN_W - 80, 72);
    lv_obj_set_size(mascotArea, 76, 90);
    lv_obj_set_style_bg_color(mascotArea, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mascotArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mascotArea, 0, 0);
    lv_obj_set_style_pad_all(mascotArea, 0, 0);
    lv_obj_clear_flag(mascotArea, LV_OBJ_FLAG_SCROLLABLE);

    display.drawMascotFrame(MASCOT_BOOT_ATTENTION, 0);
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] phase5 mascot ok");
#endif

    struct CheckItem {
        const char* label;
        bool        ok;
    };
    CheckItem checks[] = {
        { "DISPLAY",      true      },
        { "RADIO MODULE", loraOk    },
        { "STORAGE",      storageOk },
        { "SENSOR ARRAY", true      },
    };

    int checkY = 80;
    for (auto& c : checks) {
#if BOOT_SEQUENCE_VERBOSE
        Serial.printf("[BOOTSEQ] check label %s\r\n", c.label);
#endif
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, (String("> ") + c.label).c_str());
        lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x00F0FF), 0);
        lv_obj_set_pos(lbl, 8, checkY);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(120));

#if BOOT_SEQUENCE_VERBOSE
        Serial.printf("[BOOTSEQ] check tag %s\r\n", c.label);
#endif
        lv_obj_t* tag = lv_label_create(scr);
        lv_label_set_text(tag, c.ok ? "[ OK ]" : "[FAIL]");
        lv_obj_set_style_text_font(tag, FONT_SMALL, 0);
        lv_obj_set_style_text_color(tag,
            c.ok ? lv_color_hex(0x00FF9C) : lv_color_hex(0xFF003C), 0);
        lv_obj_set_pos(tag, THEME_SCREEN_W - 90, checkY);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(60));

#if BOOT_SEQUENCE_VERBOSE
        Serial.printf("[BOOTSEQ] check done %s\r\n", c.label);
#endif
        checkY += 16;
    }

#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] neural");
#endif
    lv_obj_t* neuralLbl = lv_label_create(scr);
    lv_label_set_text(neuralLbl, "");
    lv_obj_set_style_text_font(neuralLbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(neuralLbl, lv_color_hex(0xA07800), 0);
    lv_obj_set_pos(neuralLbl, 8, checkY);
    _typewrite(neuralLbl, "> NEURAL LINK........[SEARCHING]", 22);

    vTaskDelay(pdMS_TO_TICKS(800));

#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] standby mascot");
#endif
    display.drawMascotFrame(MASCOT_STANDBY, 0);
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] standby mascot ok");
#endif
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(600));

    // ── Phase 6: glitch wipe to main UI ──────────────────────
#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] wipe");
#endif
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(scr,
            i % 2 == 0 ?
            lv_color_hex(0x00F0FF) :
            lv_color_hex(0x000000), 0);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    lv_obj_t* wipeBar = lv_obj_create(scr);
    lv_obj_set_size(wipeBar, THEME_SCREEN_W, THEME_SCREEN_H);
    lv_obj_set_style_bg_color(wipeBar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wipeBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wipeBar, 0, 0);
    lv_obj_set_style_radius(wipeBar, 0, 0);
    lv_obj_clear_flag(wipeBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(wipeBar, 0, 0);

    for (int y = 0; y <= THEME_SCREEN_H; y += 8) {
        lv_obj_set_size(wipeBar, THEME_SCREEN_W, y);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(8));
    }

    lv_obj_t* blackout = lv_obj_create(scr);
    lv_obj_set_size(blackout, THEME_SCREEN_W, THEME_SCREEN_H);
    lv_obj_set_pos(blackout, 0, 0);
    lv_obj_set_style_bg_color(blackout, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(blackout, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(blackout, 0, 0);
    lv_obj_set_style_radius(blackout, 0, 0);
    lv_obj_clear_flag(blackout, LV_OBJ_FLAG_SCROLLABLE);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(30));

#if BOOT_SEQUENCE_VERBOSE
    Serial.println("[BOOTSEQ] done");
#endif
}



