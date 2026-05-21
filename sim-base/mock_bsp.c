/*
 * mock_bsp.c — host stub impls.
 */

#include "mock_bsp.h"
#include <stdio.h>

static int s_brightness = 100;

void bsp_display_brightness_set(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (s_brightness != pct) {
        s_brightness = pct;
        printf("[mock_bsp] brightness -> %d%%\n", pct);
    }
}

/* The host main() initialises LVGL+SDL directly, so this stub just
 * returns the active display. */
lv_display_t *bsp_display_start(void)
{
    return lv_display_get_default();
}
