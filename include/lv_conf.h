/**
 * lv_conf.h — LVGL v9 configuration for the T-Display S3 Pro
 *
 * We override only what matters for THIS board. Every macro we leave out
 * falls back to LVGL's built-in default (lv_conf_internal.h backfills all of
 * them), which is why this file is short instead of the ~1000-line template.
 *
 * How the build finds this file:
 *   platformio.ini has  -D LV_CONF_INCLUDE_SIMPLE  (LVGL then does
 *   `#include "lv_conf.h"`) and  -I include        (puts this folder on the
 *   compiler's search path so that include resolves here).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- Colour ------------------------------------------------------------ */
/* 16-bit RGB565 — matches the ST7796 panel we've been driving all along. */
#define LV_COLOR_DEPTH 16

/* ---- Memory ------------------------------------------------------------ */
/* Use LVGL's own allocator backed by a fixed static pool. 48 KB holds a few
 * screens of widgets comfortably; we'll bump it if allocations start to fail. */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_MEM_SIZE           (48 * 1024U)

/* ---- Tick source ------------------------------------------------------- */
/* LVGL needs to know how much time has passed. We hand it millis() at runtime
 * via lv_tick_set_cb() in setup(), so nothing to configure here. */

/* ---- Fonts ------------------------------------------------------------- */
/* Montserrat 14 is the default UI font; enable a larger one for headings. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_14

/* ---- Logging ----------------------------------------------------------- */
/* Off for now; flip to 1 (with LV_LOG_LEVEL) if we need LVGL's own diagnostics. */
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
