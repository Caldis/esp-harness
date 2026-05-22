/*
 * keys.c — three physical buttons.
 *
 * BOOT / USER are dumb GPIOs: configure as input + pull-up, read level
 * (active-low). PWR is on the AXP2101's PWRON pin, exposed via the
 * PMIC's IRQ status registers. We sample IRQ_STATUS_3 (0x4A) each
 * tick — its PWRKEY_* bits latch until written back, which gives us
 * lossless press/release detection at 20 Hz polling.
 *
 * No interrupt routing: at 50 ms tick rate, even a short press is
 * captured. Saves us a GPIO_ISR install + extra task context.
 */

#include "keys.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "keys";

#define KEY_BOOT_GPIO     GPIO_NUM_0
#define KEY_USER_GPIO     GPIO_NUM_18
#define POLL_MS           50

#define PMIC_ADDR         0x34
#define REG_IRQ_EN_1      0x40
#define REG_IRQ_EN_2      0x41
#define REG_IRQ_EN_3      0x42
#define REG_IRQ_STATUS_1  0x48
#define REG_IRQ_STATUS_2  0x49
#define REG_IRQ_STATUS_3  0x4A
/* On the AXP2101 the power-key events live in IRQ_STATUS_2 (0x49):
 *   bit 2 = PKEY short press
 *   bit 3 = PKEY long press
 *   bit 4 = PKEY negative edge (release)
 *   bit 5 = PKEY positive edge (press)
 * We treat any of those as "a press happened". 0x4A holds things
 * like watchdog / over-temperature, NOT the power key. */
#define IRQ2_PWRKEY_MASK  0x3C   /* bits 2..5 */

static i2c_master_dev_handle_t s_pmic = NULL;
static keys_state_t s_state;
static TaskHandle_t s_task = NULL;

static bool was_boot = false, was_user = false;

/* Synth-press override: when non-zero, the keys_task skips overwriting
 * the named button's `pressed` level from the real GPIO/PMIC reading.
 * Each is a tick count past which the override expires.
 * Set by keys_synth_press(), checked by keys_task. */
static volatile uint32_t s_synth_boot_until = 0;
static volatile uint32_t s_synth_user_until = 0;
static volatile uint32_t s_synth_pwr_until  = 0;

static esp_err_t pmic_read_u8(uint8_t reg, uint8_t *out)
{
    if (!s_pmic) return ESP_FAIL;
    return i2c_master_transmit_receive(s_pmic, &reg, 1, out, 1, 50);
}

static esp_err_t pmic_write_u8(uint8_t reg, uint8_t val)
{
    if (!s_pmic) return ESP_FAIL;
    uint8_t pkt[2] = { reg, val };
    return i2c_master_transmit(s_pmic, pkt, 2, 50);
}

static void keys_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now_ticks = xTaskGetTickCount();
        /* GPIO buttons — active-low (pressed = 0). */
        bool boot = (gpio_get_level(KEY_BOOT_GPIO) == 0);
        bool user = (gpio_get_level(KEY_USER_GPIO) == 0);

        if (boot && !was_boot) s_state.boot_count++;
        if (user && !was_user) s_state.user_count++;
        was_boot = boot;
        was_user = user;
        /* Real GPIO wins unless a synth override is active for this
         * button. The override is set by keys_synth_press() with an
         * expiry tick; while active, we preserve s_state.<x>_pressed
         * (which the synth set to true). After expiry the next poll
         * resumes reflecting the GPIO. */
        if (now_ticks >= s_synth_boot_until) s_state.boot_pressed = boot;
        if (now_ticks >= s_synth_user_until) s_state.user_pressed = user;
        /* pwr override: when the window expires we DON'T have a fresh
         * "real" value to fall back on (PMIC PWRKEY events are level-
         * sensed via IRQ_STATUS_2 below, applied conditionally only
         * when a real edge fires). So on expiry we explicitly drop
         * the synth's pressed=true back to false. Round-4 subagent
         * caught this — without the explicit drop, the level stayed
         * stuck at true forever. */
        if (now_ticks >= s_synth_pwr_until && s_synth_pwr_until != 0) {
            s_state.pwr_pressed = false;
            s_synth_pwr_until = 0;
        }

        /* Diagnostic: log any non-zero IRQ status the first time we
         * see one, across all three banks. Helps identify which bit
         * the AXP2101 actually flags for a PWR press on this board
         * revision (the datasheet covers multiple silicon dies). */
        uint8_t s1 = 0, sd2 = 0, s3 = 0;
        pmic_read_u8(REG_IRQ_STATUS_1, &s1);
        pmic_read_u8(REG_IRQ_STATUS_2, &sd2);
        pmic_read_u8(REG_IRQ_STATUS_3, &s3);
        static uint8_t last_s1 = 0, last_s2 = 0, last_s3 = 0;
        if (s1 != last_s1 || sd2 != last_s2 || s3 != last_s3) {
            ESP_LOGI(TAG, "IRQ_STATUS  s1=0x%02x  s2=0x%02x  s3=0x%02x",
                     s1, sd2, s3);
            last_s1 = s1; last_s2 = sd2; last_s3 = s3;
        }

        /* PMIC PWRKEY events latched in IRQ_STATUS_2 (0x49) bits 2..5.
         * Bit 5 (positive edge, press) — when seen — leaves
         * `pressed` true for one tick; bit 4 (negative edge / release)
         * flips it back. The latched flags are W1C, so we clear what
         * we just observed to arm for the next edge. Re-use the value
         * we already read in the diagnostic above. */
        {
            uint8_t evt = sd2 & IRQ2_PWRKEY_MASK;
            if (evt) {
                /* Count a press once per positive edge OR once per
                 * any-flag observation if positive-edge bit isn't
                 * available on this AXP2101 revision. */
                if (evt & ((1u << 5) | (1u << 2) | (1u << 3))) {
                    s_state.pwr_count++;
                }
                /* `pressed` reflects "currently held" — positive edge
                 * sets, negative edge clears. If a synth override is
                 * active we MUST NOT let a stray PMIC bit (e.g. a
                 * latched negative-edge from boot, or noise on the
                 * PWRON line) clear the synthesised press. The override
                 * window owns the level until it expires. */
                bool synth_active = (now_ticks < s_synth_pwr_until) &&
                                    (s_synth_pwr_until != 0);
                if (!synth_active) {
                    if (evt & (1u << 5)) s_state.pwr_pressed = true;
                    if (evt & (1u << 4)) s_state.pwr_pressed = false;
                }
                /* Always W1C the bits so the next real edge isn't
                 * masked by stale flags. */
                pmic_write_u8(REG_IRQ_STATUS_2, evt);
            }
        }
        (void)s1; (void)s3;   /* read for diagnostic only */

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool keys_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << KEY_BOOT_GPIO) | (1ULL << KEY_USER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return false;
    }

    /* Attach a device handle on the shared I²C bus for the PMIC PWRKEY
     * register. Same bus as PMIC's own driver — this is a second
     * handle on the same physical address, which i2c_master tolerates
     * (each handle has its own queue slot). */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = PMIC_ADDR,
            .scl_speed_hz    = 400000,
        };
        if (i2c_master_bus_add_device(bus, &dev, &s_pmic) != ESP_OK) {
            ESP_LOGW(TAG, "PMIC handle add failed — PWR key will read 0");
            s_pmic = NULL;
        }
    }
    if (s_pmic) {
        /* Enable PWRKEY interrupts (positive edge / negative edge /
         * short press / long press). Without this, the AXP2101 won't
         * latch the events in IRQ_STATUS_2 at all. Read-modify-write
         * so we don't disturb other event sources. */
        uint8_t en2 = 0;
        if (pmic_read_u8(REG_IRQ_EN_2, &en2) == ESP_OK) {
            uint8_t want = en2 | IRQ2_PWRKEY_MASK;
            if (want != en2) {
                pmic_write_u8(REG_IRQ_EN_2, want);
                ESP_LOGI(TAG, "IRQ_EN_2 0x%02x -> 0x%02x (PWRKEY armed)",
                         en2, want);
            } else {
                ESP_LOGI(TAG, "IRQ_EN_2 already 0x%02x (PWRKEY bits on)", en2);
            }
        }
        /* Clear stale latched events so we start from a known state. */
        pmic_write_u8(REG_IRQ_STATUS_2, IRQ2_PWRKEY_MASK);
    }

    memset(&s_state, 0, sizeof(s_state));

    BaseType_t ok = xTaskCreate(keys_task, "keys", 2560, NULL, 4, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return false;
    }
    ESP_LOGI(TAG, "keys polling GPIO0(BOOT) + GPIO18(USER) + AXP2101 PWRON");
    return true;
}

void keys_get(keys_state_t *out)
{
    if (out) memcpy(out, &s_state, sizeof(*out));
}

/* ── Synthetic press (host-driven) ───────────────────────────────── */

bool keys_synth_press(const char *which, uint32_t hold_ms)
{
    if (which == NULL) return false;
    /* Anchor the override window slightly past the requested hold so
     * even a 0-hold "quick tap" gets at least one keys_task poll
     * window (POLL_MS = 50 ms) of visible pressed=true before the
     * GPIO state takes back over. */
    uint32_t window_ms = (hold_ms == 0) ? (POLL_MS + 5) : hold_ms;
    uint32_t until = xTaskGetTickCount() + pdMS_TO_TICKS(window_ms);
    if (strcmp(which, "boot") == 0) {
        s_state.boot_count++;
        s_state.boot_pressed = true;
        s_synth_boot_until = until;
    } else if (strcmp(which, "user") == 0) {
        s_state.user_count++;
        s_state.user_pressed = true;
        s_synth_user_until = until;
    } else if (strcmp(which, "pwr") == 0) {
        s_state.pwr_count++;
        s_state.pwr_pressed = true;
        s_synth_pwr_until = until;
    } else {
        return false;
    }
    /* No spawned release task needed — the override-expiry check
     * inside keys_task is the release mechanism, and it runs every
     * POLL_MS anyway. */
    return true;
}
