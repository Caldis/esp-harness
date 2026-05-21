/*
 * ble.c — minimal NimBLE observer.
 *
 * Initialisation:
 *   nimble_port_init                — host stack
 *   ble_hs_cfg.sync_cb              — wait until controller is sync'd
 *   nimble_port_freertos_init       — runs the host task on its own thread
 *
 * Scan:
 *   ble_gap_disc(...)  with our cb
 *   accumulate unique (addr,addr_type) tuples into a static buffer
 *   stop after duration_ms via ble_gap_disc_cancel
 *
 * Threading: ble_scan is called from the console task. It blocks on a
 * semaphore that the GAP callback gives when scan completes (or
 * timeout via FreeRTOS). The host task is independent.
 */

#include "ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_heap_caps.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_bt.h"

static const char *TAG = "ble";

static bool s_inited = false;
static volatile bool s_synced = false;
static SemaphoreHandle_t s_done_sem = NULL;
static SemaphoreHandle_t s_sync_sem = NULL;

/* Static scratch shared between ble_scan and the GAP callback. The
 * console is the only caller, single-threaded, so a static struct is
 * fine for a scan-only use case. */
static struct {
    ble_device_t devices[BLE_SCAN_MAX_DEVICES];
    int          count;
    int          adv_events;
    int          max_devices;
} s_scan;

static int find_or_insert(const uint8_t addr[6], uint8_t addr_type)
{
    for (int i = 0; i < s_scan.count; ++i) {
        if (s_scan.devices[i].addr_type == addr_type &&
            memcmp(s_scan.devices[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    if (s_scan.count >= s_scan.max_devices) return -1;
    int idx = s_scan.count++;
    memset(&s_scan.devices[idx], 0, sizeof(s_scan.devices[idx]));
    memcpy(s_scan.devices[idx].addr, addr, 6);
    s_scan.devices[idx].addr_type = addr_type;
    s_scan.devices[idx].rssi = -127;
    return idx;
}

static void try_extract_name(const struct ble_hs_adv_fields *fields, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    if (fields == NULL) return;
    if (fields->name_len > 0 && fields->name != NULL) {
        size_t n = fields->name_len < cap - 1 ? fields->name_len : cap - 1;
        memcpy(out, fields->name, n);
        out[n] = '\0';
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event == NULL) return 0;

    if (event->type == BLE_GAP_EVENT_DISC) {
        s_scan.adv_events++;
        int idx = find_or_insert(event->disc.addr.val, event->disc.addr.type);
        if (idx < 0) return 0;   /* table full, drop */
        if (event->disc.rssi > s_scan.devices[idx].rssi) {
            s_scan.devices[idx].rssi = event->disc.rssi;
        }
        if (s_scan.devices[idx].name[0] == '\0') {
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
                try_extract_name(&fields, s_scan.devices[idx].name, sizeof(s_scan.devices[idx].name));
            }
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (s_done_sem) xSemaphoreGive(s_done_sem);
    }
    return 0;
}

static void on_sync(void)
{
    s_synced = true;
    if (s_sync_sem) xSemaphoreGive(s_sync_sem);
    ESP_LOGI(TAG, "host sync'd");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "controller reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "host task entered, calling nimble_port_run");
    nimble_port_run();
    ESP_LOGW(TAG, "nimble_port_run returned — host loop exited");
    nimble_port_freertos_deinit();
}

bool ble_init(void)
{
    if (s_inited) return true;

    if (s_done_sem == NULL) {
        s_done_sem = xSemaphoreCreateBinary();
        if (s_done_sem == NULL) return false;
    }
    if (s_sync_sem == NULL) {
        s_sync_sem = xSemaphoreCreateBinary();
        if (s_sync_sem == NULL) return false;
    }

    /* Register callbacks BEFORE port init — sync may fire during the
     * init handshake on the same thread, so registering afterwards
     * misses the event and we wait forever. */
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return false;
    }
    /* Skip nimble_port_freertos_init — it doesn't check xTaskCreate's
     * return, and on this board the task creation silently fails. The
     * symptom is "7MB heap free" but task allocation failing because
     * FreeRTOS stacks live in *internal* SRAM (not PSRAM), and at
     * boot internal SRAM is fragmented by LVGL + audio buffers. A 4 KB
     * stack fits where 8 KB doesn't. */
    TaskHandle_t th = NULL;
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "pre-host: internal SRAM free=%u largest_block=%u",
             (unsigned)free_int, (unsigned)largest_int);
    BaseType_t ok = xTaskCreate(host_task, "nimble_host", 4096, NULL, 6, &th);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(host) failed (%d)", (int)ok);
        return false;
    }
    s_inited = true;
    ESP_LOGI(TAG, "ble observer ready");
    return true;
}

bool ble_is_up(void) { return s_inited; }

bool ble_release_memory(void)
{
    /* If BLE is initialised, full deinit chain handles the release. */
    if (s_inited) return ble_deinit();
    /* Otherwise just release the controller-side pool. The controller
     * was never enabled, so we can skip disable/deinit and jump
     * straight to the memory release. */
    esp_err_t mr = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    if (mr != ESP_OK && mr != ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE = already released; treat as idempotent OK. */
        ESP_LOGW(TAG, "esp_bt_mem_release (no-init path): %s",
                 esp_err_to_name(mr));
        return false;
    }
    ESP_LOGI(TAG, "ble controller pool released (no-init path)");
    return true;
}

bool ble_deinit(void)
{
    if (!s_inited) return true;
    /* Cancel any in-flight discovery so the controller stops talking. */
    if (s_synced) {
        ble_gap_disc_cancel();
    }
    /* NOTE: we deliberately DO NOT call `nimble_port_deinit`. It chains
     * into `ble_hs_deinit` → `ble_sm_deinit`, but the security manager
     * isn't compiled in (observer-only build). The host stays in
     * memory after this; the freedom we need is on the controller side
     * (~25 KB of internal SRAM), and that's what gets reclaimed by
     * disabling + deiniting the BT controller below. WiFi can then
     * init successfully. The next `ble_init` will not work after this
     * — that's the trade-off we make explicit in the scene UI. */
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    /* Release the BLE memory pool back to the heap allocator. Without
     * this, the BT controller's internal-SRAM reservation stays
     * permanently checked out, and WiFi's `esf_buf_setup_static`
     * fails. After release, BLE cannot be re-initialised — that's the
     * one-way trade-off we accept in exchange for WiFi headroom. */
    esp_err_t mr = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    if (mr != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_mem_release: %s", esp_err_to_name(mr));
    }
    s_inited = false;
    s_synced = false;
    ESP_LOGI(TAG, "ble controller released (memory returned to heap)");
    return true;
}

int ble_scan(ble_device_t *out, int max_out, int duration_ms, int *total_adv)
{
    if (!s_inited) {
        if (!ble_init()) return -1;
    }
    if (out == NULL || max_out <= 0) return -1;
    if (duration_ms < 100)   duration_ms = 100;
    if (duration_ms > 10000) duration_ms = 10000;

    /* Wait for the host to sync with the controller before issuing any
     * GAP API call. The controller usually settles within ~50ms of
     * boot, but the first ?ble after a cold start may arrive earlier
     * than that on a fast harness — so block here up to 2s. */
    if (!s_synced) {
        if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "host did not sync within 2s");
            return -1;
        }
    }

    s_scan.count = 0;
    s_scan.adv_events = 0;
    s_scan.max_devices = max_out < BLE_SCAN_MAX_DEVICES ? max_out : BLE_SCAN_MAX_DEVICES;

    /* Drain any stale completion signal so we don't return immediately. */
    xSemaphoreTake(s_done_sem, 0);

    struct ble_gap_disc_params params = {
        .itvl = 0x0010,        /* 10 ms interval */
        .window = 0x0010,      /* 10 ms window */
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,          /* no SCAN_REQ → quieter, lower power */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_ms, &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc: %d", rc);
        return -1;
    }

    /* Wait for DISC_COMPLETE, plus 500ms slack for the controller. */
    TickType_t wait = pdMS_TO_TICKS(duration_ms + 500);
    if (xSemaphoreTake(s_done_sem, wait) != pdTRUE) {
        /* Hard stop if the controller never signalled. */
        ble_gap_disc_cancel();
    }

    int copied = s_scan.count < max_out ? s_scan.count : max_out;
    memcpy(out, s_scan.devices, copied * sizeof(out[0]));
    if (total_adv) *total_adv = s_scan.adv_events;
    return copied;
}
