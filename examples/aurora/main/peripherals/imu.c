/*
 * imu.c — QMI8658C wiring.
 *
 * The Waveshare component's qmi8658_init already configures accel for
 * 8 G @ 1000 Hz and gyro for 512 dps @ 1000 Hz. Any post-init call to
 * set_accel_range / set_accel_odr puts the chip into a saturated state
 * (raw bytes pegged at 0x7fff / 0x8000 with STATUS0=0 forever). Don't
 * touch CTRL2/CTRL3 after init — use the defaults.
 *
 * Output unit caveat: with the default accel_unit_mps2=false, the lib
 * returns *milli-g* (~±1000 for 1 g), not g. We divide by 1000 in the
 * getter so the rest of the firmware sees clean ±1.0 g values.
 *
 * Calibration: a one-shot "capture current pose as neutral" — the scene
 * sees the delta from whatever orientation the board was in at boot.
 * No assumption about Z=1g flat-on-table.
 */

#include "imu.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "bsp/esp-bsp.h"   /* bsp_i2c_get_handle() */
#include "qmi8658.h"

static const char *TAG = "imu";

#define IMU_ADDR             QMI8658_ADDRESS_HIGH     /* 0x6B */
#define POLL_INTERVAL_MS     10                       /* 100 Hz */
#define EMA_ALPHA            0.18f                    /* visual smoothing */
#define CALIB_SAMPLES        25
#define CALIB_DELAY_MS       8

static qmi8658_dev_t s_dev;
static bool          s_ready = false;
static TaskHandle_t  s_task = NULL;

/* Cached filtered readings (g). Atomic writes/reads of 32-bit floats are
 * safe on Xtensa LX7 — no lock needed for the getters. */
static volatile float s_ax = 0.0f, s_ay = 0.0f, s_az = 0.0f;
static volatile float s_gx = 0.0f, s_gy = 0.0f, s_gz = 0.0f;
static volatile float s_temp_c = 0.0f;

/* Neutral pose captured during init — whatever orientation the board
 * is in at boot, that's "zero tilt" for the visual layer. */
static float s_off_x = 0.0f, s_off_y = 0.0f, s_off_z = 0.0f;

/* Axis remap: sensor → screen frame. Identity for now; we'll tune
 * empirically once we can hold the board and read screen output.
 *
 * The mapping is:
 *   screen_x =  m[0][0]*sx + m[0][1]*sy
 *   screen_y =  m[1][0]*sx + m[1][1]*sy
 *   screen_z =  m[2][2]*sz
 *
 * Updated 2026-05-17 after a board-in-hand test session — TBD. */
static const float s_remap[3][3] = {
    { 1.0f,  0.0f, 0.0f },
    { 0.0f,  1.0f, 0.0f },
    { 0.0f,  0.0f, 1.0f },
};

static inline void apply_remap(float in_x, float in_y, float in_z,
                                float *out_x, float *out_y, float *out_z)
{
    *out_x = s_remap[0][0] * in_x + s_remap[0][1] * in_y + s_remap[0][2] * in_z;
    *out_y = s_remap[1][0] * in_x + s_remap[1][1] * in_y + s_remap[1][2] * in_z;
    *out_z = s_remap[2][0] * in_x + s_remap[2][1] * in_y + s_remap[2][2] * in_z;
}

/* Read accel and convert mg → g in one shot. */
static inline esp_err_t read_accel_g(float *gx, float *gy, float *gz)
{
    float mx, my, mz;
    esp_err_t err = qmi8658_read_accel(&s_dev, &mx, &my, &mz);
    if (err != ESP_OK) return err;
    *gx = mx * 0.001f;
    *gy = my * 0.001f;
    *gz = mz * 0.001f;
    return ESP_OK;
}

static void imu_task(void *arg)
{
    (void)arg;
    /* Neutral-pose calibration: average N samples and use whatever the
     * board orientation is at boot as "zero tilt". The visual layer
     * sees deltas from this baseline. */
    float sx = 0, sy = 0, sz = 0;
    int taken = 0;
    for (int i = 0; i < CALIB_SAMPLES; ++i) {
        float ax, ay, az;
        if (read_accel_g(&ax, &ay, &az) == ESP_OK) {
            sx += ax; sy += ay; sz += az;
            taken++;
        }
        vTaskDelay(pdMS_TO_TICKS(CALIB_DELAY_MS));
    }
    if (taken > 0) {
        s_off_x = sx / taken;
        s_off_y = sy / taken;
        s_off_z = sz / taken;
    }
    ESP_LOGI(TAG, "calibration: neutral pose = (%.3f, %.3f, %.3f) g  |g|=%.3f",
             s_off_x, s_off_y, s_off_z,
             sqrtf(s_off_x * s_off_x + s_off_y * s_off_y + s_off_z * s_off_z));

    s_ready = true;

    int temp_subsample = 0;
    while (1) {
        float raw_x, raw_y, raw_z;
        if (read_accel_g(&raw_x, &raw_y, &raw_z) == ESP_OK) {
            float dx = raw_x - s_off_x;
            float dy = raw_y - s_off_y;
            float dz = raw_z - s_off_z;
            float fx, fy, fz;
            apply_remap(dx, dy, dz, &fx, &fy, &fz);
            s_ax = s_ax * (1.0f - EMA_ALPHA) + fx * EMA_ALPHA;
            s_ay = s_ay * (1.0f - EMA_ALPHA) + fy * EMA_ALPHA;
            s_az = s_az * (1.0f - EMA_ALPHA) + fz * EMA_ALPHA;
        }
        /* Gyro: same chip, similar read. Lib returns dps when
         * gyro_unit_rads=false (the default — set by qmi8658_init). EMA
         * the same way so static-board readings settle near 0. */
        float gx_raw = 0, gy_raw = 0, gz_raw = 0;
        if (qmi8658_read_gyro(&s_dev, &gx_raw, &gy_raw, &gz_raw) == ESP_OK) {
            s_gx = s_gx * (1.0f - EMA_ALPHA) + gx_raw * EMA_ALPHA;
            s_gy = s_gy * (1.0f - EMA_ALPHA) + gy_raw * EMA_ALPHA;
            s_gz = s_gz * (1.0f - EMA_ALPHA) + gz_raw * EMA_ALPHA;
        }
        /* Temperature changes slowly — read once per second. */
        if (++temp_subsample >= 100) {  /* 100 × 10 ms = 1 s */
            temp_subsample = 0;
            float t_c = 0.0f;
            if (qmi8658_read_temp(&s_dev, &t_c) == ESP_OK) {
                s_temp_c = t_c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

bool imu_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus not yet initialised");
        return false;
    }
    /* The QMI8658 needs ~150 ms from VDD-stable to first valid read.
     * bsp_display_start (which brings up I2C + touch) takes ~1.5 s, so
     * we're already well past that by the time imu_init runs — but a
     * small pad doesn't hurt. */
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = qmi8658_init(&s_dev, bus, IMU_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qmi8658_init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Sanity: WHO_AM_I should be 0x05 for this chip. */
    uint8_t who = 0;
    if (qmi8658_get_who_am_i(&s_dev, &who) != ESP_OK || who != 0x05) {
        ESP_LOGE(TAG, "unexpected WHO_AM_I = 0x%02x (want 0x05)", who);
    } else {
        ESP_LOGI(TAG, "WHO_AM_I = 0x%02x", who);
    }

    /* qmi8658_init has already set range/odr and enabled both sensors.
     * Wait a couple of ODR periods for the first valid samples. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Sampler task in its own stack. 4 KB is plenty. */
    BaseType_t ok = xTaskCreate(imu_task, "imu", 4096, NULL, 4, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }

    ESP_LOGI(TAG, "imu_init ok (calibration in progress)");
    return true;
}

bool imu_is_ready(void) { return s_ready; }

void imu_get_gyro(float *gx, float *gy, float *gz)
{
    if (gx) *gx = s_gx;
    if (gy) *gy = s_gy;
    if (gz) *gz = s_gz;
}

float imu_get_temp_c(void) { return s_temp_c; }

void imu_get_accel(float *ax, float *ay, float *az)
{
    if (ax) *ax = s_ax;
    if (ay) *ay = s_ay;
    if (az) *az = s_az;
}
