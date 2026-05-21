/*
 * Scene XIV · System — read-only system dashboard.
 *
 * Aurora's diagnostic surface, all in one place. Three columns:
 *
 *   COMPUTE                  MEMORY                  THERMAL / RADIO
 *   240 MHz × 2 cores        SRAM   54 / 320 KB      SoC    42 °C
 *   chip rev 0.2             PSRAM  6.9 / 8.0 MB     PMIC   38 °C
 *   reset: PANIC                                     IMU    29 °C
 *   uptime 03:14:20
 *
 *   ────────────────────────────────────────────────────────────
 *   IDENTITY
 *   aurora 0f36f93   IDF v6.0.1   built May 18 01:11
 *   WiFi  a4:cb:8f:d7:56:f4
 *   BT    a4:cb:8f:d7:56:f6
 *
 * All numbers refresh at 1 Hz. The static block (chip rev / IDF /
 * MACs / reset reason) is computed once at boot.
 */

#include "harness/scene_framework.h"
#include "lvgl.h"
#include "peripherals/system.h"
#include "peripherals/pmic.h"
#include "peripherals/imu.h"
#include "esp_log.h"
#include <stdio.h>

#define ACCENT       0x6AB7E8     /* clinical sky blue */

typedef struct {
    lv_obj_t *roman;
    /* Compute column */
    lv_obj_t *cpu;          /* "240 MHz x 2" */
    lv_obj_t *chip;         /* "rev 0.2 / S3" */
    lv_obj_t *reset;        /* "reset: PANIC" */
    lv_obj_t *uptime;       /* "up 03:14:20" */
    /* Memory column */
    lv_obj_t *sram;         /* "SRAM 54 / 320 KB" */
    lv_obj_t *sram_min;     /* "min  21 KB" */
    lv_obj_t *psram;        /* "PSRAM 6.9 / 8.0 MB" */
    lv_obj_t *psram_min;    /* "min  6.8 MB" */
    /* Thermal column */
    lv_obj_t *t_soc;        /* "SoC   42 °C" */
    lv_obj_t *t_pmic;       /* "PMIC  38 °C" */
    lv_obj_t *t_imu;        /* "IMU   29 °C" */
    lv_obj_t *flash;        /* "flash 16 MB" */
    /* Identity row */
    lv_obj_t *id_line1;     /* "aurora 0f36f93 · IDF v6.0.1" */
    lv_obj_t *id_line2;     /* "built ..." */
    lv_obj_t *id_mac_wifi;
    lv_obj_t *id_mac_bt;
    lv_timer_t *timer;
} sys_state_t;

static void fmt_kb(char *buf, size_t cap, uint32_t bytes)
{
    if (bytes >= 1024U * 1024U)
        snprintf(buf, cap, "%.1f MB", bytes / 1048576.0f);
    else
        snprintf(buf, cap, "%lu KB", (unsigned long)(bytes / 1024U));
}

static void sys_tick(lv_timer_t *t)
{
    sys_state_t *st = (sys_state_t *)lv_timer_get_user_data(t);
    if (!st) return;

    system_info_t s;
    system_get(&s);

    /* 128-byte scratch: identity lines combine app_name (32) +
     * app_version (32) and similar, comfortably under this size. */
    char buf[128];

    /* COMPUTE */
    snprintf(buf, sizeof(buf), "%d MHz  x %d", s.cpu_freq_mhz, s.chip_cores);
    lv_label_set_text(st->cpu, buf);
    snprintf(buf, sizeof(buf), "rev %d.%d  S3",
             s.chip_revision / 100, s.chip_revision % 100);
    lv_label_set_text(st->chip, buf);
    snprintf(buf, sizeof(buf), "reset: %s", s.reset_reason);
    lv_label_set_text(st->reset, buf);
    uint32_t up_s = (uint32_t)(s.uptime_ms / 1000U);
    uint32_t h = up_s / 3600U, m = (up_s % 3600U) / 60U, sec = up_s % 60U;
    snprintf(buf, sizeof(buf), "up %02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    lv_label_set_text(st->uptime, buf);

    /* MEMORY — SRAM total is fixed ~327680 bytes available to app after
     * IDF carves out its own pools. Show both free and largest contiguous
     * block; the latter is what task creation actually cares about. */
    char a[24], b[24];
    fmt_kb(a, sizeof(a), s.heap_internal_free);
    fmt_kb(b, sizeof(b), s.heap_internal_largest);
    snprintf(buf, sizeof(buf), "SRAM  %s free, %s max", a, b);
    lv_label_set_text(st->sram, buf);
    fmt_kb(a, sizeof(a), s.heap_internal_min);
    snprintf(buf, sizeof(buf), "min ever %s", a);
    lv_label_set_text(st->sram_min, buf);

    fmt_kb(a, sizeof(a), s.heap_psram_free);
    fmt_kb(b, sizeof(b), s.heap_psram_largest);
    snprintf(buf, sizeof(buf), "PSRAM %s free, %s max", a, b);
    lv_label_set_text(st->psram, buf);
    fmt_kb(a, sizeof(a), s.heap_psram_min);
    snprintf(buf, sizeof(buf), "min ever %s", a);
    lv_label_set_text(st->psram_min, buf);

    /* THERMAL */
    snprintf(buf, sizeof(buf), "SoC  %.1f C", s.soc_temp_c);
    lv_label_set_text(st->t_soc, buf);

    pmic_state_t p;
    pmic_get(&p);
    if (p.tdie_c > -40 && p.tdie_c < 125) {
        snprintf(buf, sizeof(buf), "PMIC %.1f C", p.tdie_c);
    } else {
        snprintf(buf, sizeof(buf), "PMIC -");
    }
    lv_label_set_text(st->t_pmic, buf);

    float t_imu = imu_get_temp_c();
    if (t_imu > -40 && t_imu < 125) {
        snprintf(buf, sizeof(buf), "IMU  %.1f C", t_imu);
    } else {
        snprintf(buf, sizeof(buf), "IMU  -");
    }
    lv_label_set_text(st->t_imu, buf);

    snprintf(buf, sizeof(buf), "flash %lu MB", (unsigned long)s.flash_size_mb);
    lv_label_set_text(st->flash, buf);

    /* IDENTITY — set once but cheap to refresh and might pick up late-
     * initialised fields like the BT MAC. */
    snprintf(buf, sizeof(buf), "%s  %s", s.app_name, s.app_version);
    lv_label_set_text(st->id_line1, buf);
    snprintf(buf, sizeof(buf), "IDF %s  ELF %s", s.idf_version, s.elf_sha256_short);
    lv_label_set_text(st->id_line2, buf);
    snprintf(buf, sizeof(buf), "WiFi %s", s.wifi_mac);
    lv_label_set_text(st->id_mac_wifi, buf);
    snprintf(buf, sizeof(buf), "BT   %s", s.bt_mac);
    lv_label_set_text(st->id_mac_bt, buf);
}

static lv_obj_t *make_text(lv_obj_t *parent, const lv_font_t *font,
                            int opa, const char *initial)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_opa(lbl, opa, 0);
    lv_label_set_text(lbl, initial);
    return lbl;
}

static void sys_init(scene_t *s, lv_obj_t *parent)
{
    sys_state_t *st = lv_malloc_zeroed(sizeof(*st));
    s->user_data = st;

    /* Roman XIV */
    st->roman = make_text(parent, &lv_font_montserrat_22, LV_OPA_60, "XIV");
    lv_obj_set_style_text_letter_space(st->roman, 6, 0);
    lv_obj_align(st->roman, LV_ALIGN_TOP_MID, 0, 75);

    /* Three columns at y = -50..40, three rows of two-three labels each.
     * Layout co-ordinates chosen to fit the round 466×466 bezel. */
    const int COL_X_L = -120;
    const int COL_X_C = 0;
    const int COL_X_R = 120;
    const int ROW_TOP = -60;
    const int LINE = 18;

    /* COMPUTE (left) */
    st->cpu     = make_text(parent, &lv_font_montserrat_14, LV_OPA_90, "");
    st->chip    = make_text(parent, &lv_font_montserrat_14, LV_OPA_70, "");
    st->reset   = make_text(parent, &lv_font_montserrat_14, LV_OPA_70, "");
    st->uptime  = make_text(parent, &lv_font_montserrat_14, LV_OPA_70, "");
    lv_obj_align(st->cpu,    LV_ALIGN_CENTER, COL_X_L, ROW_TOP);
    lv_obj_align(st->chip,   LV_ALIGN_CENTER, COL_X_L, ROW_TOP + LINE);
    lv_obj_align(st->reset,  LV_ALIGN_CENTER, COL_X_L, ROW_TOP + LINE * 2);
    lv_obj_align(st->uptime, LV_ALIGN_CENTER, COL_X_L, ROW_TOP + LINE * 3);

    /* MEMORY (centre) */
    st->sram      = make_text(parent, &lv_font_montserrat_14, LV_OPA_90, "");
    st->sram_min  = make_text(parent, &lv_font_montserrat_12, LV_OPA_50, "");
    st->psram     = make_text(parent, &lv_font_montserrat_14, LV_OPA_90, "");
    st->psram_min = make_text(parent, &lv_font_montserrat_12, LV_OPA_50, "");
    lv_obj_align(st->sram,      LV_ALIGN_CENTER, COL_X_C, ROW_TOP);
    lv_obj_align(st->sram_min,  LV_ALIGN_CENTER, COL_X_C, ROW_TOP + LINE);
    lv_obj_align(st->psram,     LV_ALIGN_CENTER, COL_X_C, ROW_TOP + LINE * 2);
    lv_obj_align(st->psram_min, LV_ALIGN_CENTER, COL_X_C, ROW_TOP + LINE * 3);

    /* THERMAL (right) */
    st->t_soc   = make_text(parent, &lv_font_montserrat_14, LV_OPA_90, "");
    st->t_pmic  = make_text(parent, &lv_font_montserrat_14, LV_OPA_70, "");
    st->t_imu   = make_text(parent, &lv_font_montserrat_14, LV_OPA_70, "");
    st->flash   = make_text(parent, &lv_font_montserrat_12, LV_OPA_50, "");
    lv_obj_align(st->t_soc,  LV_ALIGN_CENTER, COL_X_R, ROW_TOP);
    lv_obj_align(st->t_pmic, LV_ALIGN_CENTER, COL_X_R, ROW_TOP + LINE);
    lv_obj_align(st->t_imu,  LV_ALIGN_CENTER, COL_X_R, ROW_TOP + LINE * 2);
    lv_obj_align(st->flash,  LV_ALIGN_CENTER, COL_X_R, ROW_TOP + LINE * 3);

    /* IDENTITY (bottom block) */
    st->id_line1     = make_text(parent, &lv_font_montserrat_14, LV_OPA_80, "");
    st->id_line2     = make_text(parent, &lv_font_montserrat_12, LV_OPA_50, "");
    st->id_mac_wifi  = make_text(parent, &lv_font_montserrat_12, LV_OPA_50, "");
    st->id_mac_bt    = make_text(parent, &lv_font_montserrat_12, LV_OPA_50, "");
    lv_obj_align(st->id_line1,    LV_ALIGN_CENTER, 0, 30);
    lv_obj_align(st->id_line2,    LV_ALIGN_CENTER, 0, 48);
    lv_obj_align(st->id_mac_wifi, LV_ALIGN_CENTER, 0, 64);
    lv_obj_align(st->id_mac_bt,   LV_ALIGN_CENTER, 0, 78);

    /* 1 Hz refresh — everything we display moves slowly. */
    st->timer = lv_timer_create(sys_tick, 1000, st);
    lv_timer_pause(st->timer);
    sys_tick(st->timer);   /* first paint */
}

static void sys_on_show(scene_t *s)
{
    sys_state_t *st = (sys_state_t *)s->user_data;
    if (st && st->timer) {
        lv_timer_resume(st->timer);
        sys_tick(st->timer);
    }
}
static void sys_on_hide(scene_t *s)
{
    sys_state_t *st = (sys_state_t *)s->user_data;
    if (st && st->timer) lv_timer_pause(st->timer);
}

scene_t scene_system = {
    .id           = "system",
    .display_name = "XIV. System",
    .accent       = LV_COLOR_MAKE(0x6A, 0xB7, 0xE8),
    .description  = "SoC dashboard: CPU/cores/heap/PSRAM/temps/MAC/IDF/reset",
    .tags         = "diagnostic,system,readonly",
    .init         = sys_init,
    .on_show      = sys_on_show,
    .on_hide      = sys_on_hide,
};
