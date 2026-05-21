/*
 * settings.c — NVS-backed persistent config with write throttling.
 *
 * Boot path: settings_init reads "aurora" namespace, populates the
 * cache, and starts a low-priority background task. Each setter
 * updates the cache + sets a dirty flag. The bg task wakes whenever
 * flagged, sleeps 5 s, and commits in one batch. Five seconds is a
 * compromise: long enough to deduplicate fast slider spins, short
 * enough that a quick power-cycle after one tweak still saves.
 *
 * NVS is mounted by aurora_main during the early-init block (so we
 * piggy-back on the WiFi/BLE setup), no separate nvs_flash_init here.
 */

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";
#define NVS_NS              "aurora"
#define K_VOLUME            "volume"
#define K_BRIGHTNESS        "brightness"
#define K_LAST_SCENE        "last_scene"
#define COMMIT_DELAY_MS     5000

static settings_t s_cache = {
    .volume_pct = 70,
    .brightness_pct = 100,
    .last_scene_idx = 0,
};

static SemaphoreHandle_t s_dirty_sem = NULL;
static volatile bool     s_dirty = false;

static void load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    int32_t v;
    if (nvs_get_i32(h, K_VOLUME,     &v) == ESP_OK) s_cache.volume_pct      = v;
    if (nvs_get_i32(h, K_BRIGHTNESS, &v) == ESP_OK) s_cache.brightness_pct  = v;
    if (nvs_get_i32(h, K_LAST_SCENE, &v) == ESP_OK) s_cache.last_scene_idx  = v;
    nvs_close(h);
    ESP_LOGI(TAG, "restored: vol=%d brightness=%d last_scene=%d",
             s_cache.volume_pct, s_cache.brightness_pct, s_cache.last_scene_idx);
}

static void commit_now(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open RW failed; settings not persisted this round");
        return;
    }
    nvs_set_i32(h, K_VOLUME,      s_cache.volume_pct);
    nvs_set_i32(h, K_BRIGHTNESS,  s_cache.brightness_pct);
    nvs_set_i32(h, K_LAST_SCENE,  s_cache.last_scene_idx);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "committed: vol=%d brightness=%d scene=%d",
                 s_cache.volume_pct, s_cache.brightness_pct,
                 s_cache.last_scene_idx);
    } else {
        ESP_LOGW(TAG, "nvs_commit: %s", esp_err_to_name(err));
    }
    s_dirty = false;
}

static void settings_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wait indefinitely for any change. */
        xSemaphoreTake(s_dirty_sem, portMAX_DELAY);
        /* Throttle: sleep 5 s. If more changes come in during the sleep,
         * the semaphore is given again — but we already collapsed them
         * into the cache, so we just wake briefly. */
        vTaskDelay(pdMS_TO_TICKS(COMMIT_DELAY_MS));
        /* Drain any extra "dirty" semaphore signals collapsed during
         * the sleep; we'll commit them in one shot. */
        while (xSemaphoreTake(s_dirty_sem, 0) == pdTRUE) {}
        if (s_dirty) commit_now();
    }
}

bool settings_init(void)
{
    s_dirty_sem = xSemaphoreCreateBinary();
    if (!s_dirty_sem) return false;
    load_from_nvs();
    BaseType_t ok = xTaskCreate(settings_task, "settings", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    return true;
}

void settings_get(settings_t *out)
{
    if (out) *out = s_cache;
}

static inline void mark_dirty(void)
{
    s_dirty = true;
    if (s_dirty_sem) xSemaphoreGive(s_dirty_sem);
}

void settings_set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (s_cache.volume_pct == pct) return;
    s_cache.volume_pct = pct;
    mark_dirty();
}
void settings_set_brightness(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (s_cache.brightness_pct == pct) return;
    s_cache.brightness_pct = pct;
    mark_dirty();
}
void settings_set_last_scene(int idx)
{
    if (s_cache.last_scene_idx == idx) return;
    s_cache.last_scene_idx = idx;
    mark_dirty();
}

void settings_flush(void)
{
    if (s_dirty) commit_now();
}
