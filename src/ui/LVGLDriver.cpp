
#include "LVGLDriver.h"


// This file is the only normal runtime path that may touch TFT directly.
// All higher-level UI must render through LVGL objects.


TFT_eSPI* LVGLDriver::_tft = nullptr;
uint8_t* LVGLDriver::_drawBuf = nullptr;
void* LVGLDriver::_lvglPoolMem = nullptr;
lv_mem_pool_t LVGLDriver::_lvglPool = nullptr;

#define DRAW_BUF_SIZE (THEME_SCREEN_W * 20 * sizeof(lv_color_t))
static uint8_t draw_buf[DRAW_BUF_SIZE];

void LVGLDriver::begin(TFT_eSPI* tft) {
    _tft = tft;
    lv_init();
    lv_display_t* disp = lv_display_create(THEME_SCREEN_W, THEME_SCREEN_H);
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_buffers(disp, draw_buf, nullptr,
                           DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void LVGLDriver::tick() {
    lv_tick_inc(1);
}

bool LVGLDriver::hasExtendedPool() {
    return true;
}

void LVGLDriver::flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    _tft->startWrite();
    _tft->setAddrWindow(area->x1, area->y1, w, h);
    _tft->pushColors((uint16_t*)px_map, w * h, true);
    _tft->endWrite();
    lv_display_flush_ready(disp);
}


