/*
 * lv_conf.h — host build LVGL config.
 *
 * Mirrors the target's important options (Montserrat font sizes etc)
 * but enables SDL backend instead of the CO5300 driver. Keep in sync
 * with `managed_components/lvgl__lvgl/lv_conf_template.h` defaults
 * where it matters; deviations are commented inline.
 *
 * If you add new font sizes / widgets to a scene, mirror them here so
 * the host build compiles.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ── Display ────────────────────────────────────────────────────────── */
/* 466×466 matches the Waveshare AMOLED's resolution. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* ── Memory ─────────────────────────────────────────────────────────── */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (256U * 1024U)

/* ── HAL: SDL backend ──────────────────────────────────────────────── */
#define LV_USE_SDL              1
#define LV_SDL_BUF_COUNT        1
#define LV_SDL_FULLSCREEN       0
#define LV_SDL_DIRECT_EXIT      1
#define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER

/* ── Tick ──────────────────────────────────────────────────────────── */
#define LV_TICK_CUSTOM 0   /* host main loop calls lv_tick_inc */

/* ── Logging ───────────────────────────────────────────────────────── */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* ── Asserts ───────────────────────────────────────────────────────── */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/* ── Fonts (mirror target — scenes use these sizes) ───────────────── */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_14

/* ── Widgets we use ────────────────────────────────────────────────── */
#define LV_USE_LABEL 1
#define LV_USE_BTN   1
#define LV_USE_IMG   1
#define LV_USE_ARC   1
#define LV_USE_BAR   1
#define LV_USE_LINE  1
#define LV_USE_CANVAS 1

/* ── Themes ────────────────────────────────────────────────────────── */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

/* ── Image decoders / snapshot ─────────────────────────────────────── */
#define LV_USE_SNAPSHOT 1

#endif /* LV_CONF_H */
