/*
 * pmic.c — minimal AXP2101 driver, register-level.
 *
 * We deliberately skip XPowersLib (C++ wrapper, separate I²C bus setup,
 * menuconfig knobs) — Aurora only needs four reads, doing them by hand
 * keeps the dependency tree shallow and matches the IMU's pattern.
 *
 * Register subset:
 *   0x00 STATUS1  — VBUS / battery presence flags
 *   0x01 STATUS2  — charge state in low 3 bits, direction in bits 5–6
 *   0x34 / 0x35   — battery voltage ADC (high5 + low8 = mV directly)
 *   0xA4          — fuel-gauge percent (0..100)
 *
 * Bit semantics inferred from XPowersAXP2101.hpp on the lewisxhe lib —
 * cross-checked against the AXP2101 datasheet (POWERINFOSYS).
 */

#include "pmic.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"   /* bsp_i2c_get_handle */

static const char *TAG = "pmic";

#define PMIC_ADDR             0x34
#define POLL_INTERVAL_MS      1000

#define REG_STATUS1           0x00
#define REG_STATUS2           0x01
#define REG_ADC_CH_CTRL       0x30   /* ADC channel enable bits */
#define REG_VBAT_H            0x34   /* high 5 bits in low nibble */
#define REG_VBAT_L            0x35   /* low 8 bits */
#define REG_VBUS_H            0x38   /* USB input voltage ADC, same layout */
#define REG_VBUS_L            0x39
#define REG_BAT_PERCENT       0xA4

/* ADC channel enable bits in REG_ADC_CH_CTRL:
 *   bit 0 VBAT
 *   bit 1 TS
 *   bit 2 VBUS
 *   bit 3 TDIE
 * Aurora needs VBAT + VBUS. Touch the others if temperature ever lands
 * on the dashboard. */
#define ADC_EN_MASK           ((1u << 0) | (1u << 2) | (1u << 3))
#define REG_TDIE_H            0x3C
#define REG_TDIE_L            0x3D

/* Rolling samples for %/min rate calculation. 60 slots × 1 s sample
 * interval = 1-minute window. */
#define RATE_HIST_LEN         60

static i2c_master_dev_handle_t s_dev = NULL;
static pmic_state_t s_state = { .ready = false };
static TaskHandle_t s_task = NULL;

/* Rolling history for charge-rate estimation. We track BOTH percent
 * and battery voltage because the AXP2101 fuel gauge reports integer
 * percent and only steps once per several minutes during a steady
 * charge — but voltage changes continuously. The dashboard displays
 * voltage rate as the responsive signal and percent rate as the
 * long-run average. */
typedef struct {
    int64_t ts_ms;
    int     percent;
    int     mv;
} rate_sample_t;
static rate_sample_t s_hist[RATE_HIST_LEN];
static int s_hist_head = 0;
static int s_hist_count = 0;

static esp_err_t pmic_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t pmic_read_u8(uint8_t reg, uint8_t *out)
{
    return pmic_read(reg, out, 1);
}

static pmic_charge_state_t decode_charge(uint8_t status2)
{
    /* low 3 bits: 0=tri, 1=pre, 2=cc, 3=cv, 4=done, 5=NA, 6/7=stop */
    switch (status2 & 0x07) {
        case 0: return PMIC_CHG_TRICKLE;
        case 1: return PMIC_CHG_PRE;
        case 2: return PMIC_CHG_CONST_CUR;
        case 3: return PMIC_CHG_CONST_VOLT;
        case 4: return PMIC_CHG_DONE;
        case 5: return PMIC_CHG_NA;
        default: return PMIC_CHG_OFF;
    }
}

/* Convert raw high+low pair to mV. AXP2101's ADC results pack a 14-bit
 * value as `high[5:0]` + `low[7:0]` (high nibble has the top bits).
 * The lib's `readRegisterH5L8` uses (high & 0x1F) << 8 | low — same
 * layout for VBAT and VBUS channels. */
static inline int adc_pair_to_mv(uint8_t hi, uint8_t lo)
{
    return ((int)(hi & 0x1F) << 8) | (int)lo;
}

/* Push the current sample and compute both %/min and mV/min over the
 * full history window. dt is the same for both. */
static void compute_rates(int64_t now_ms, int percent, int mv,
                          float *out_pct_per_min, float *out_mv_per_min)
{
    *out_pct_per_min = 0.0f;
    *out_mv_per_min  = 0.0f;

    s_hist[s_hist_head].ts_ms = now_ms;
    s_hist[s_hist_head].percent = percent;
    s_hist[s_hist_head].mv = mv;
    s_hist_head = (s_hist_head + 1) % RATE_HIST_LEN;
    if (s_hist_count < RATE_HIST_LEN) s_hist_count++;

    if (s_hist_count < 2) return;
    int oldest_idx = (s_hist_head - s_hist_count + RATE_HIST_LEN) % RATE_HIST_LEN;
    int64_t dt_ms = now_ms - s_hist[oldest_idx].ts_ms;
    if (dt_ms < 5000) return;   /* < 5 s history → too noisy */

    int dp = percent - s_hist[oldest_idx].percent;
    int dv = mv      - s_hist[oldest_idx].mv;
    *out_pct_per_min = (float)dp * 60000.0f / (float)dt_ms;
    *out_mv_per_min  = (float)dv * 60000.0f / (float)dt_ms;
}

static void sample_once(pmic_state_t *out)
{
    uint8_t s1 = 0, s2 = 0, vh = 0, vl = 0, pct = 0, ub_h = 0, ub_l = 0;
    if (pmic_read_u8(REG_STATUS1,     &s1)   != ESP_OK) return;
    if (pmic_read_u8(REG_STATUS2,     &s2)   != ESP_OK) return;
    if (pmic_read_u8(REG_VBAT_H,      &vh)   != ESP_OK) return;
    if (pmic_read_u8(REG_VBAT_L,      &vl)   != ESP_OK) return;
    if (pmic_read_u8(REG_BAT_PERCENT, &pct)  != ESP_OK) return;
    /* VBUS voltage — only meaningful when the channel is enabled in 0x30 */
    pmic_read_u8(REG_VBUS_H, &ub_h);
    pmic_read_u8(REG_VBUS_L, &ub_l);
    /* TDIE (PMIC die temperature). AXP2101 returns a 12-bit value with
     * a slope of 0.10625 °C / LSB and an offset of 22.5 °C per the
     * datasheet (the offset varies between revisions; values look
     * sane on this board). */
    uint8_t td_h = 0, td_l = 0;
    pmic_read_u8(REG_TDIE_H, &td_h);
    pmic_read_u8(REG_TDIE_L, &td_l);
    int td_raw = ((int)(td_h & 0x3F) << 8) | (int)td_l;
    /* Formula from AXP2101 reference docs: T[°C] = 22.5 - raw * 0.0625
     * (signed, the chip uses an inverted scale). Verified to track
     * room temp ~+10°C above when on-charge. */
    float tdie_c = 22.5f - (float)td_raw * 0.0625f;
    /* Sanity-clamp wild ADC readings during boot transients. */
    if (tdie_c < -40.0f || tdie_c > 125.0f) tdie_c = 0.0f;

    /* STATUS1 bit semantics (from XPowers driver):
     *   bit 5  VBUS good
     *   bit 3  battery present
     * STATUS2 bits 5..6 = charge direction; 01 = charging
     *   bit 3 (active-low) for VBUS-in is also used, but bit 5 of
     *   STATUS1 is the more reliable indicator. */
    out->vbus_in  = (s1 & (1u << 5)) != 0;
    out->battery  = (s1 & (1u << 3)) != 0;
    out->charge   = out->battery ? decode_charge(s2) : PMIC_CHG_OFF;
    out->percent  = out->battery ? (int)pct : -1;
    out->voltage_mv = out->battery ? adc_pair_to_mv(vh, vl) : 0;
    out->vbus_voltage_mv = out->vbus_in ? adc_pair_to_mv(ub_h, ub_l) : 0;
    out->tdie_c = tdie_c;

    /* Rates are only meaningful when a battery is connected. Otherwise
     * leave both at zero and reset history. */
    if (out->battery) {
        compute_rates(esp_timer_get_time() / 1000,
                      out->percent, out->voltage_mv,
                      &out->rate_pct_per_min, &out->rate_mv_per_min);
    } else {
        out->rate_pct_per_min = 0.0f;
        out->rate_mv_per_min  = 0.0f;
        s_hist_count = 0;
    }
    out->ready    = true;
}

static void pmic_task(void *arg)
{
    (void)arg;
    pmic_state_t tmp;
    while (1) {
        memcpy(&tmp, &s_state, sizeof(tmp));
        sample_once(&tmp);
        s_state = tmp;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

bool pmic_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus not yet initialised");
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PMIC_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Probe — a single status read confirms the chip ACKs. */
    uint8_t s1 = 0;
    if (pmic_read_u8(REG_STATUS1, &s1) != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 did not respond to STATUS1 read");
        return false;
    }
    ESP_LOGI(TAG, "AXP2101 alive  STATUS1=0x%02x", s1);

    /* Make sure VBAT + VBUS ADC channels are enabled. Read-modify-write
     * so we don't disturb whatever the BSP/bootloader configured for
     * the temperature channels. */
    uint8_t adc = 0;
    if (pmic_read_u8(REG_ADC_CH_CTRL, &adc) == ESP_OK) {
        uint8_t want = adc | (uint8_t)ADC_EN_MASK;
        if (want != adc) {
            uint8_t pkt[2] = { REG_ADC_CH_CTRL, want };
            i2c_master_transmit(s_dev, pkt, 2, 50);
            ESP_LOGI(TAG, "ADC channels enable 0x%02x -> 0x%02x", adc, want);
        }
    }

    /* First synchronous sample so callers see real values immediately. */
    sample_once(&s_state);
    ESP_LOGI(TAG, "initial: vbus=%d bat=%d chg=%s %d%% %dmV",
             s_state.vbus_in, s_state.battery,
             pmic_charge_label(s_state.charge),
             s_state.percent, s_state.voltage_mv);

    BaseType_t ok = xTaskCreate(pmic_task, "pmic", 3072, NULL, 3, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }
    return true;
}

bool pmic_is_ready(void) { return s_state.ready; }

void pmic_get(pmic_state_t *out)
{
    if (out) {
        memcpy(out, &s_state, sizeof(*out));
    }
}

const char *pmic_charge_label(pmic_charge_state_t s)
{
    switch (s) {
        case PMIC_CHG_TRICKLE:    return "trickle";
        case PMIC_CHG_PRE:        return "pre";
        case PMIC_CHG_CONST_CUR:  return "fast";
        case PMIC_CHG_CONST_VOLT: return "taper";
        case PMIC_CHG_DONE:       return "done";
        case PMIC_CHG_NA:         return "n/a";
        case PMIC_CHG_OFF:
        default:                  return "off";
    }
}
