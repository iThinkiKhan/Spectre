

#pragma once
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "Theme.h"

class LVGLDriver {
public:
    static void begin(TFT_eSPI* tft);
    static void tick();   // call every ms from a timer or task
    static void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static bool hasExtendedPool();

private:
    static constexpr size_t DRAW_BUF_SIZE = THEME_SCREEN_W * 20 * sizeof(lv_color_t);
    static constexpr size_t LVGL_PSRAM_POOL_SIZE = 64 * 1024U;
    static constexpr size_t LVGL_INTERNAL_POOL_FALLBACK_SIZE = 24 * 1024U;

    static TFT_eSPI* _tft;
    static uint8_t* _drawBuf;
    static void* _lvglPoolMem;
    static lv_mem_pool_t _lvglPool;
};



