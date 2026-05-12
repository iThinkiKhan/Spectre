


#if 1 /*Set it to "1" to enable content*/

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_CUSTOM 0
// Keep just enough static pool for lv_init() overhead (~1-2 KB in practice).
// The real working pool (64 KB) is added from PSRAM by LVGLDriver::begin()
// immediately after lv_init() via lv_mem_add_pool().  Shrinking this from 64 KB
// recovers ~56 KB of internal BSS for BLE and other DMA-capable allocations.
#define LV_MEM_SIZE (8 * 1024U)
// CRITICAL: TLSF computes FL_INDEX_MAX (its internal bitmask ceiling) at
// compile time as TLSF_LOG2_CEIL(LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE).
// Without this, TLSF is physically capped at 8 KB and lv_mem_add_pool()
// silently returns NULL for any block larger than LV_MEM_SIZE.
#define LV_MEM_POOL_EXPAND_SIZE (64 * 1024U)

/*====================
   HAL SETTINGS
 *====================*/
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/*====================
   FONT USAGE
 *====================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/* Custom font declarations */
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(orbitron_bold_uncompressed_16) \
    LV_FONT_DECLARE(orbitron_bold_uncompressed_32) \
    LV_FONT_DECLARE(sharetech_uncompressed_14) \
    LV_FONT_DECLARE(sharetech_uncompressed_16) \
    LV_FONT_DECLARE(spacemono_bold_14) \
    LV_FONT_DECLARE(spacemono_bold_16)

/* Font format settings */
#define LV_FONT_FMT_TXT_LARGE 1
#define LV_USE_FONT_SUBPX 0
#define LV_FONT_SUBPX_BGR 0

/*====================
   FEATURE CONFIGURATION
 *====================*/
#define LV_USE_DRAW_SW_ASM 0
#define LV_DRAW_SW_COMPLEX 1
#define LV_USE_OBSERVER 0
#define LV_USE_XML 0
#define LV_USE_CANVAS 1

/*====================
   WIDGET USAGE
 *====================*/
#define LV_USE_ARC        0
#define LV_USE_BAR        1
#define LV_USE_BTN        0
#define LV_USE_CANVAS     1
#define LV_USE_IMG        0
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_TEXTAREA   0

#define LV_USE_BTNMATRIX  0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0
#define LV_USE_SWITCH     0
#define LV_USE_TABLE      0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0


/*====================
   THEME
 *====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/*====================
   LOGGING
 *====================*/
#define LV_USE_LOG 0

#endif /*LV_CONF_H*/
#endif /*End of "Content enable"*/




