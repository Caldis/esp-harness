/*
 * aurora — generative-art companion for the Waveshare ESP32-S3-Touch-AMOLED-2.16.
 *
 * Boot order:
 *   1. esp_log already on
 *   2. bsp_display_start()        — brings up CO5300 + LVGL + CST9217 touch
 *   3. console_protocol_init()    — start serial command parser
 *   4. ui_shell_init(N)           — chrome on lv_layer_top
 *   5. scene_fw_init(screen)      — frame timer + container management
 *   6. scene_fw_register(...)     — each scene in display order
 *   7. harness_commands_register()— ?stat / ?dump / tap / swipe / scene
 *   8. install touch handler that switches scenes on tap
 *   9. heartbeat task for AI observability
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "harness/console_protocol.h"
#include "harness/scene_framework.h"
#include "harness/harness_commands.h"
#include "peripherals/imu.h"
#include "peripherals/pmic.h"
#include "peripherals/audio.h"
#include "peripherals/sdcard.h"
#include "peripherals/ble.h"
#include "peripherals/keys.h"
#include "peripherals/system.h"
#include "peripherals/settings.h"
#include "bsp/esp-bsp.h"
#include "ui_shell.h"
#include "scenes/scenes.h"

static const char *TAG = "aurora";

#include "harness/default_cmds.h"  /* harness_record_frame from aurora-harness */

/* Single source of truth for "scene changed" side-effects: UI chrome
 * sync, NVS persistence, host EVT emission. Registered as the framework
 * change listener once at boot; fires both for tap-driven `scene_fw_next()`
 * and for console-driven `scene_fw_show(N)`. */
static void on_scene_changed(int idx, const scene_t *current)
{
    if (!current) return;
    ui_shell_set_active(idx, current->display_name);
    settings_set_last_scene(idx);
    console_send_evt("scene_changed idx=%d id=%s", idx, current->id);
}

static void on_screen_short_click(lv_event_t *e)
{
    (void)e;
    /* Short tap: advance scene. Side-effects flow through on_scene_changed. */
    scene_fw_next();
}

static void on_screen_long_press(lv_event_t *e)
{
    (void)e;
    /* Held ≥ 400 ms: dispatch the current scene's action callback.
     * Scenes opt in by setting on_long_press; non-interactive scenes
     * (Halo / Grid / Bloom) leave it NULL and the gesture is a no-op. */
    const scene_t *c = scene_fw_current();
    if (c && c->on_long_press) {
        /* The on_long_press callback runs on the LVGL task (which holds
         * the LVGL mutex here), so scenes can freely manipulate widgets.
         * Anything that blocks for >1 frame should defer via a worker
         * task — see scene_echo's async tone playback. */
        c->on_long_press((scene_t *)c);
        console_send_evt("scene_action idx=%d id=%s",
                         scene_fw_current_index(), c->id);
    }
}

static void on_screen_release(lv_event_t *e)
{
    (void)e;
    /* Fires on every finger-lift. Most scenes ignore it (on_release
     * NULL); Listen uses it to stop an in-progress recording when the
     * user releases the press-and-hold. */
    const scene_t *c = scene_fw_current();
    if (c && c->on_release) {
        c->on_release((scene_t *)c);
    }
}

static void fps_update_cb(lv_timer_t *t)
{
    (void)t;
    /* fps_cached is updated by harness_record_frame() each frame_tick.
     * Read it via ?stat — but for the UI shell we maintain a local
     * counter so the shell stays self-contained. */
    static uint32_t last_frames = 0;
    static int64_t  last_us = 0;
    int64_t now = esp_timer_get_time();
    if (last_us == 0) {
        last_us = now;
        return;
    }
    /* We don't have direct access to the frame counter here; approximate
     * via lv_timer measure. Instead, the shell pulls FPS from elapsed
     * draws. For now, just show a placeholder if we don't have data. */
    float fps = 1000000.0f / (float)(now - last_us);  /* meaningless first */
    (void)fps; (void)last_frames;
    last_us = now;
}

/* Per-frame tick that LVGL calls — emits a frame counter for ?stat
 * and for the UI shell FPS reading. */
static void frame_counter_cb(lv_timer_t *t)
{
    (void)t;
    harness_record_frame();
}

void app_main(void)
{
    ESP_LOGI(TAG, "aurora starting");

    /* 0. Bring up infrastructure shared by BLE / WiFi / PHY.
     * NVS provides RF calibration storage (PHY logs a warning otherwise).
     * esp_netif + default event loop must exist before any code creates
     * a wifi/eth netif. WiFi lazy-init calls these too but does so AFTER
     * BLE has already initialised — and on a fresh PHY-no-NVS boot,
     * BLE's PHY calibration runs without storage, and the subsequent
     * netif creation has occasionally asserted. Doing it once here makes
     * the order deterministic. */
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
    }
    esp_netif_init();
    esp_err_t evt_err = esp_event_loop_create_default();
    if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event_loop_create_default: %s", esp_err_to_name(evt_err));
    }

    /* 0b. NVS-backed settings — read BEFORE display unlock so the
     * restored scene is applied during the same render barrier as the
     * scene_fw registration. Otherwise scene 0 paints for ~1 frame
     * before we jump to the last scene, producing a visible flicker. */
    settings_init();
    settings_t prefs;
    settings_get(&prefs);

    /* 0c. SoC introspection — no display dependency; safe to do early. */
    system_init();

    /* 1. Display + touch + LVGL */
    bsp_display_start();
    bsp_display_brightness_set(prefs.brightness_pct);
    ESP_LOGI(TAG, "bsp_display_start ok (brightness=%d%%)", prefs.brightness_pct);

    /* 2. Console protocol (serial command parser) */
    console_protocol_init();

    /* 3. Build UI under LVGL lock. */
    bsp_display_lock(-1);

    /* v1.4 — twenty scenes. Live list at runtime: `scene list` →
     * SCENES JSON. Human-readable summary: docs/scenes-map.md.
     * XIX Notify is the reference impl for harness_toast(); XX Track
     * is the reference impl for harness_progress(). */
    const int kSceneCount = 20;
    ui_shell_init(kSceneCount);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    /* SHORT_CLICKED rather than RELEASED so a held-then-released touch
     * doesn't also count as a scene-advance. LONG_PRESSED handler runs
     * the per-scene action. */
    lv_obj_add_event_cb(scr, on_screen_short_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, on_screen_long_press,  LV_EVENT_LONG_PRESSED,  NULL);
    lv_obj_add_event_cb(scr, on_screen_release,     LV_EVENT_RELEASED,      NULL);

    scene_fw_init(scr);
    /* Register the chrome-sync listener BEFORE the first scene_fw_show
     * call so the auto-shown scene 0 still gets ui_shell synced. */
    scene_fw_set_change_listener(on_scene_changed);
    scene_fw_register(&scene_halo);
    scene_fw_register(&scene_grid);
    scene_fw_register(&scene_bloom);
    scene_fw_register(&scene_tilt);
    scene_fw_register(&scene_pulse);
    scene_fw_register(&scene_echo);
    scene_fw_register(&scene_vault);
    scene_fw_register(&scene_whisper);
    scene_fw_register(&scene_spectrum);
    scene_fw_register(&scene_cell);
    scene_fw_register(&scene_keys);
    scene_fw_register(&scene_listen);
    scene_fw_register(&scene_tone);
    scene_fw_register(&scene_system);
    scene_fw_register(&scene_glow);
    scene_fw_register(&scene_spin);
    scene_fw_register(&scene_survey);
    scene_fw_register(&scene_sniff);
    scene_fw_register(&scene_notify);
    scene_fw_register(&scene_track);

    /* Restore last visited scene BEFORE leaving the LVGL lock — this way
     * the auto-shown scene 0 (from scene_fw_register's first call) never
     * renders. We're still under bsp_display_lock so LVGL has not drawn
     * a frame since registration. The on_scene_changed listener already
     * synced the ui_shell chrome for both transitions (the auto-show
     * during the first scene_fw_register and the restore below). */
    if (prefs.last_scene_idx > 0 && prefs.last_scene_idx < kSceneCount) {
        scene_fw_show(prefs.last_scene_idx);
    }

    /* Per-frame counter for ?stat — runs on LVGL task. */
    lv_timer_create(frame_counter_cb, 16, NULL);

    bsp_display_unlock();

    /* 4. Application commands depend on display + scene_fw being ready. */
    harness_commands_register();

    /* 5. IMU — non-fatal if it fails; Tilt scene falls back to gravity=(0,0,1g). */
    if (!imu_init()) {
        ESP_LOGW(TAG, "IMU init failed -Tilt scene will show neutral pose");
    }

    /* 6. PMIC — non-fatal if it fails; Pulse scene shows neutral state. */
    if (!pmic_init()) {
        ESP_LOGW(TAG, "PMIC init failed -Pulse scene will read all-zero");
    }

    /* 7. Audio — non-fatal if it fails; ?audio command will report -1. */
    if (!audio_init()) {
        ESP_LOGW(TAG, "Audio init failed -?audio commands will be no-ops");
    } else {
        audio_set_volume(prefs.volume_pct);
    }

    /* 8. SD card — non-fatal; no card inserted is the most common case
     * during development, so we expect this to fail-soft. */
    if (!sdcard_init()) {
        ESP_LOGI(TAG, "SD card not mounted (no card or read error)");
    }

    /* 8b. Physical buttons — BOOT (GPIO0) + USER (GPIO18) + PWR via
     * AXP2101. Used by scene_keys live indicator and ?keys command. */
    if (!keys_init()) {
        ESP_LOGW(TAG, "keys init failed - physical button scene will read 0");
    }

    /* 8c. (Settings + system + last_scene restore done in step 0b/0c —
     * before the LVGL lock — so the restored scene never shows a flicker
     * of scene 0.) */

    /* 9. BLE — **NOT** initialised at boot. Both radios are lazy: the
     * first scene you enter (Whisper or Spectrum) brings up its radio
     * and wins the internal-SRAM pool. Booting with neither active
     * keeps the SRAM budget reservable for whichever the user picks
     * first.
     *
     * Trade-off: switching between BLE and WiFi at runtime needs to
     * fully release one to make room for the other. `ble_deinit`
     * releases the controller pool via `esp_bt_mem_release(BTDM)`
     * but the NimBLE host's PSRAM-routed allocations stay (harmless).
     * WiFi → BLE requires a reboot today. */

    ESP_LOGI(TAG, "aurora ready %d scene(s)", kSceneCount);

    /* 5. Heartbeat loop so AI can confirm liveness over long captures. */
    uint32_t hb = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "heartbeat #%" PRIu32 " scene=%d",
                 hb++, scene_fw_current_index());
    }
}
