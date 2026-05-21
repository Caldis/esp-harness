/*
 * mock_peripherals.c — minimum-effort stubs.
 *
 * The point is to let target scenes' #include lines resolve and any
 * direct getter calls return something non-crashy. No simulation
 * fidelity is promised; per-scene fake data should be added on demand.
 */

#include "mock_peripherals.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <SDL2/SDL.h>

/* ── IMU ─────────────────────────────────────────────────────────── */
/* SDL mouse → fake accel: mouse offset from window centre maps to a
 * tilt vector. Lets Scene IV Tilt actually respond to mouse drag in
 * the sim, and gives Spin a nudgeable readout (mouse motion → gyro). */
bool imu_init(void) { return true; }

static void mouse_to_tilt(float *ax, float *ay)
{
    int mx, my, ww = 466, wh = 466;
    SDL_Window *win = SDL_GetMouseFocus();
    if (win) SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetMouseState(&mx, &my);
    /* Normalise to [-1, 1] then scale to ~0.7 g max. */
    float nx = ((float)mx / (float)ww - 0.5f) * 2.0f;
    float ny = ((float)my / (float)wh - 0.5f) * 2.0f;
    if (nx >  1.0f) nx =  1.0f; if (nx < -1.0f) nx = -1.0f;
    if (ny >  1.0f) ny =  1.0f; if (ny < -1.0f) ny = -1.0f;
    if (ax) *ax = nx * 0.7f;
    if (ay) *ay = ny * 0.7f;
}

void imu_get_accel(float *ax, float *ay, float *az)
{
    float x = 0, y = 0;
    mouse_to_tilt(&x, &y);
    if (ax) *ax = x;
    if (ay) *ay = y;
    /* Keep |a| ≈ 1 g — z fills in the remainder. */
    float r = x * x + y * y;
    if (r > 0.95f) r = 0.95f;
    if (az) *az = sqrtf(1.0f - r);
}

void imu_get_gyro(float *gx, float *gy, float *gz)
{
    /* Derive a synthetic rate from frame-to-frame tilt delta. Crude
     * but enough to see the Spin bars wiggle when the user drags. */
    static float last_x = 0, last_y = 0;
    static Uint32 last_t = 0;
    float x, y;
    mouse_to_tilt(&x, &y);
    Uint32 now = SDL_GetTicks();
    float dt = last_t == 0 ? 0.05f : (float)(now - last_t) / 1000.0f;
    if (dt < 0.001f) dt = 0.001f;
    /* dps ≈ delta tilt × 100 — purely heuristic, just to make Spin alive */
    float gxv = (x - last_x) / dt * 100.0f;
    float gyv = (y - last_y) / dt * 100.0f;
    last_x = x; last_y = y; last_t = now;
    if (gx) *gx = gxv;
    if (gy) *gy = gyv;
    if (gz) *gz = 0.0f;
}
float imu_get_temp_c(void) { return 27.0f; }
bool imu_is_ready(void)    { return true; }

/* ── PMIC ────────────────────────────────────────────────────────── */
bool pmic_init(void)     { return true; }
bool pmic_is_ready(void) { return true; }
void pmic_get(pmic_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ready = true;
    out->battery = true;
    out->vbus_in = true;
    out->charge = PMIC_CHG_DONE;
    out->percent = 87;       /* not 100% so scene_cell shows the rate field */
    out->voltage_mv = 4135;
    out->vbus_voltage_mv = 5012;
    out->tdie_c = 36.5f;
    out->rate_pct_per_min = 0.18f;
    out->rate_mv_per_min = 2.3f;
}
const char *pmic_charge_label(pmic_charge_state_t s)
{
    switch (s) {
        case PMIC_CHG_OFF:        return "off";
        case PMIC_CHG_TRICKLE:    return "trickle";
        case PMIC_CHG_PRE:        return "pre";
        case PMIC_CHG_CONST_CUR:  return "fast";
        case PMIC_CHG_CONST_VOLT: return "taper";
        case PMIC_CHG_DONE:       return "done";
        default:                   return "?";
    }
}

/* ── Audio ───────────────────────────────────────────────────────── */
static int s_volume_pct = 70;
bool audio_init(void)              { return true; }
void audio_set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_volume_pct = pct;
}
int  audio_get_volume(void)        { return s_volume_pct; }
bool audio_play_tone_async(int hz, int duration_ms, int volume_pct)
{
    fprintf(stderr, "[mock_audio] tone %dHz %dms vol=%d%% (no-op)\n",
            hz, duration_ms, volume_pct);
    return true;
}

/* ── WiFi ────────────────────────────────────────────────────────── */
int  wifi_scan(wifi_ap_t *out, int max_out, int timeout_ms)
{
    (void)out; (void)max_out; (void)timeout_ms;
    return 0;
}
const char *wifi_auth_label(uint8_t m) { (void)m; return "open"; }

/* ── BLE ─────────────────────────────────────────────────────────── */
bool ble_init(void)   { return true; }
bool ble_deinit(void) { return true; }
bool ble_is_up(void)  { return false; }
int  ble_scan(ble_device_t *out, int max_out, int dur_ms, int *adv)
{
    (void)out; (void)max_out; (void)dur_ms;
    if (adv) *adv = 0;
    return 0;
}

/* ── SD ──────────────────────────────────────────────────────────── */
bool sdcard_init(void)        { return false; }

/* ── Keys ────────────────────────────────────────────────────────── */
static keys_state_t s_keys = {0};

void mock_keys_set_boot(bool pressed)
{
    if (pressed && !s_keys.boot_pressed) s_keys.boot_count++;
    s_keys.boot_pressed = pressed;
}
void mock_keys_set_user(bool pressed)
{
    if (pressed && !s_keys.user_pressed) s_keys.user_count++;
    s_keys.user_pressed = pressed;
}
bool keys_init(void)         { return true; }
void keys_get(keys_state_t *out) { if (out) *out = s_keys; }

/* ── System ──────────────────────────────────────────────────────── */
static system_info_t s_sys = {
    .cpu_freq_mhz   = 240,
    .xtal_mhz       = 40,
    .chip_revision  = 200,
    .chip_cores     = 2,
    .heap_internal_free = 100 * 1024,
    .heap_psram_free    = 8 * 1024 * 1024,
    .flash_size_mb  = 16,
    .soc_temp_c     = 30.0f,
    .wifi_mac       = "aa:bb:cc:dd:ee:01",
    .bt_mac         = "aa:bb:cc:dd:ee:02",
    .idf_version    = "host-sim",
    .reset_reason   = "HOST_SIM",
    .app_name       = "aurora-sim",
    .app_version    = "host-1.1",
    .elf_sha256_short = "hostsimstub00",
    .static_inited  = true,
};
bool system_init(void) { return true; }
void system_get(system_info_t *out) { if (out) *out = s_sys; }

/* ── Settings ────────────────────────────────────────────────────── */
static settings_t s_settings = {
    .volume_pct     = 70,
    .brightness_pct = 100,
    .last_scene_idx = 0,
};
bool settings_init(void) { return true; }
void settings_get(settings_t *out) { if (out) *out = s_settings; }
void settings_set_volume(int p)     { if (p<0)p=0; if (p>100)p=100; s_settings.volume_pct=p; }
void settings_set_brightness(int p) { if (p<0)p=0; if (p>100)p=100; s_settings.brightness_pct=p; }
void settings_set_last_scene(int i) { s_settings.last_scene_idx = i; }
void settings_flush(void) {}
