

#include "LVGLDriver.h"
#include <esp_heap_caps.h>


// This file is the only normal runtime path that may touch TFT directly.
// All higher-level UI must render through LVGL objects.
//
// Memory strategy
// ───────────────
// lv_conf.h sets LV_MEM_SIZE to 8 KB (down from 64 KB) so LVGL's builtin
// static BSS pool is minimal — just enough for lv_init() overhead.
// begin() immediately extends the allocator with a 64 KB PSRAM slab via
// lv_mem_add_pool(), restoring full working capacity in PSRAM instead.
//
// The LVGL draw buffer (one 320×20 tile) is allocated from internal
// DMA-capable RAM.  It is no longer a static BSS array (saving 12.8 KB of
// internal BSS), but it stays in internal RAM so the SPI DMA path is
// unconditionally safe.


TFT_eSPI* LVGLDriver::_tft = nullptr;
uint8_t* LVGLDriver::_drawBuf = nullptr;
void* LVGLDriver::_lvglPoolMem = nullptr;
lv_mem_pool_t LVGLDriver::_lvglPool = nullptr;

void LVGLDriver::begin(TFT_eSPI* tft) {
    _tft = tft;

    // ── Draw buffer: internal DMA-capable RAM only ───────────────────────────
    // SPI DMA must be able to read this buffer directly.  Keeping it out of
    // static BSS (dynamic allocation) still frees the BSS slot, and forcing
    // MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL guarantees the DMA path is safe.
    _drawBuf = static_cast<uint8_t*>(
        heap_caps_malloc(DRAW_BUF_SIZE,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    configASSERT(_drawBuf != nullptr);   // fatal: nowhere to render

    // ── LVGL init — uses the small 8 KB static BSS pool only ─────────────────
    lv_init();

    // ── Extend allocator with a 64 KB PSRAM pool ─────────────────────────────
    // This replaces the 64 KB that was removed from the static BSS pool in
    // lv_conf.h.  All subsequent LVGL object / style / animation allocations
    // will draw from this PSRAM slab rather than from internal SRAM.
    _lvglPoolMem = heap_caps_malloc(LVGL_PSRAM_POOL_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (_lvglPoolMem) {
        _lvglPool = lv_mem_add_pool(_lvglPoolMem, LVGL_PSRAM_POOL_SIZE);
        if (!_lvglPool) {
            // lv_mem_add_pool returned NULL — most likely TLSF_MAX_POOL_SIZE is
            // smaller than LVGL_PSRAM_POOL_SIZE.  Check LV_MEM_POOL_EXPAND_SIZE
            // in lv_conf.h: it must be >= LVGL_PSRAM_POOL_SIZE (64 KB).
            Serial.println("[LVGL] ERROR: lv_mem_add_pool failed — TLSF size ceiling too low");
        }
    } else {
        // PSRAM unavailable — fall back to a smaller internal pool so the
        // device still boots, but log loudly so it isn't missed.
        _lvglPoolMem = heap_caps_malloc(LVGL_INTERNAL_POOL_FALLBACK_SIZE,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (_lvglPoolMem) {
            _lvglPool = lv_mem_add_pool(_lvglPoolMem,
                                        LVGL_INTERNAL_POOL_FALLBACK_SIZE);
        }
        Serial.println("[LVGL] WARNING: PSRAM pool alloc failed; using internal fallback");
    }

    // ── Display registration ──────────────────────────────────────────────────
    lv_display_t* disp = lv_display_create(THEME_SCREEN_W, THEME_SCREEN_H);
    lv_display_set_flush_cb(disp, flush);
    lv_display_set_buffers(disp, _drawBuf, nullptr,
                           DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void LVGLDriver::tick() {
    lv_tick_inc(1);
}

bool LVGLDriver::hasExtendedPool() {
    return _lvglPool != nullptr && _lvglPoolMem != nullptr;
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


