/*
 * Scene XI · Keys — physical button live indicator.
 *
 * Three columns side by side, in the user-requested order:
 *
 *      BOOT          PWR          USER
 *       ⬤             ⬤             ⬤
 *      12             3             27
 *
 * Each column is a label + a status pill + a press counter.
 *
 * BOOT / USER are dumb GPIOs — we drive their pills from the live
 * `pressed` level (filled while held). PWR is event-driven via the
 * AXP2101 PWRKEY IRQ flags, which on this board only latch the
 * negative-edge bit reliably (positive-edge never fires). That means
 * we can't tell "currently held" for PWR — only "an event happened".
 * Workaround: flash the PWR pill for 400 ms each time the count
 * advances, so a press still produces visible feedback.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/keys.h"
#include "esp_log.h"
#include <stdio.h>

#define ACCENT       0x9EE493   /* fresh mint */
#define COL_W        130
#define COL_GAP      20
#define PILL_W       54
#define PILL_H       32
#define FLASH_MS     400        /* PWR pill stays lit this long per edge */

typedef struct {
    lv_obj_t *label;
    lv_obj_t *pill;
    lv_obj_t *count;
} key_col_t;

typedef struct {
    lv_obj_t *roman;
    key_col_t boot;
    key_col_t pwr;
    key_col_t user;
    lv_obj_t *hint;
    uint32_t  last_pwr_count;
    uint32_t  pwr_flash_until_ms;
    lv_timer_t *timer;
} keys_scene_t;

static lv_obj_t *make_pill(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, PILL_W, PILL_H);
    lv_obj_set_style_radius(p, PILL_H / 2, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static void make_column(lv_obj_t *parent, key_col_t *col,
                        const char *label_txt, int x_offset)
{
    /* Centred top-down stack inside this column.  Anchor everything
     * to the centre of the screen at the requested X offset so we
     * stay symmetric on the round display. */
    col->label = lv_label_create(parent);
    lv_obj_set_style_text_font(col->label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(col->label, 2, 0);
    lv_obj_set_style_text_color(col->label, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(col->label, LV_OPA_80, 0);
    lv_label_set_text(col->label, label_txt);
    lv_obj_align(col->label, LV_ALIGN_CENTER, x_offset, -34);

    col->pill = make_pill(parent);
    lv_obj_align(col->pill, LV_ALIGN_CENTER, x_offset, 0);

    col->count = lv_label_create(parent);
    lv_obj_set_style_text_font(col->count, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(col->count, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(col->count, LV_OPA_70, 0);
    lv_label_set_text(col->count, "0");
    lv_obj_align(col->count, LV_ALIGN_CENTER, x_offset, 36);
}

static void apply_pill(key_col_t *col, bool lit, uint32_t count)
{
    lv_obj_set_style_bg_opa(col->pill, lit ? LV_OPA_90 : LV_OPA_0, 0);
    lv_obj_set_style_border_opa(col->pill, lit ? LV_OPA_COVER : LV_OPA_50, 0);
    char buf[12];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)count);
    lv_label_set_text(col->count, buf);
}

static void keys_tick(lv_timer_t *t)
{
    keys_scene_t *st = (keys_scene_t *)lv_timer_get_user_data(t);
    if (!st) return;
    keys_state_t k;
    keys_get(&k);

    /* PWR: flash on count change because the AXP2101 only latches the
     * release edge — `pressed` is therefore unreliable. We detect a
     * count step and arm a 400 ms decay window. */
    uint32_t now_ms = lv_tick_get();
    if (k.pwr_count != st->last_pwr_count) {
        st->pwr_flash_until_ms = now_ms + FLASH_MS;
        st->last_pwr_count = k.pwr_count;
    }
    bool pwr_lit = (int32_t)(st->pwr_flash_until_ms - now_ms) > 0;

    apply_pill(&st->boot, k.boot_pressed, k.boot_count);
    apply_pill(&st->pwr,  pwr_lit,        k.pwr_count);
    apply_pill(&st->user, k.user_pressed, k.user_count);
}

static void keys_scene_init(scene_t *s, lv_obj_t *parent)
{
    keys_scene_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman XI. */
    st->roman = lv_label_create(parent);
    lv_obj_set_style_text_font(st->roman, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_set_style_text_color(st->roman, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->roman, LV_OPA_60, 0);
    lv_label_set_text(st->roman, "XI");
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 100);

    /* Three columns: BOOT | PWR | USER. Spaced ~140 px on centre so
     * they sit comfortably inside the 466 px round bezel. */
    make_column(parent, &st->boot, "BOOT", -(COL_W + COL_GAP));   /* -150 */
    make_column(parent, &st->pwr,  "PWR",  0);
    make_column(parent, &st->user, "USER", (COL_W + COL_GAP));    /* +150 */

    /* Hint at the bottom. */
    st->hint = lv_label_create(parent);
    lv_obj_set_style_text_font(st->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st->hint, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(st->hint, LV_OPA_50, 0);
    lv_label_set_text(st->hint, "press the on-board buttons");
    lv_obj_align(st->hint, LV_ALIGN_CENTER, 0, 90);

    /* 50 ms refresh — same as the keys_task poll cadence. */
    st->timer = lv_timer_create(keys_tick, 50, st);
    lv_timer_pause(st->timer);
    keys_tick(st->timer);
}

static void keys_on_show(scene_t *s)
{
    keys_scene_t *st = (keys_scene_t *)s->user_data;
    if (st && st->timer) {
        lv_timer_resume(st->timer);
        keys_tick(st->timer);
    }
}
static void keys_on_hide(scene_t *s)
{
    keys_scene_t *st = (keys_scene_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_keys = {
    .id           = "keys",
    .display_name = "XI. Keys",
    .accent       = LV_COLOR_MAKE(0x9E, 0xE4, 0x93),
    .init         = keys_scene_init,
    .on_show      = keys_on_show,
    .on_hide      = keys_on_hide,
};
