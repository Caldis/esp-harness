/*
 * pmic.h — AXP2101 power management IC on the shared I²C bus.
 *
 * Same bus as IMU + touch (SDA=15 SCL=14), 7-bit address 0x34.
 *
 * What we read (and only what we read — the AXP2101 has dozens of
 * voltage rails, GPIOs, interrupt sources, etc. that Aurora doesn't
 * touch):
 *   - battery voltage (mV)
 *   - battery state-of-charge (0–100 %)
 *   - charging status (off / trickle / fast / done)
 *   - VBUS present (USB-C plugged in)
 *
 * A background task polls at 1 Hz (battery state changes slowly). The
 * console `?power` command and Scene V "Pulse" both read the cached
 * values via the lock-free getter.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PMIC_CHG_OFF        = 0,  /* not charging */
    PMIC_CHG_TRICKLE    = 1,  /* very-low-voltage trickle */
    PMIC_CHG_PRE        = 2,  /* pre-charge */
    PMIC_CHG_CONST_CUR  = 3,  /* constant-current fast charge */
    PMIC_CHG_CONST_VOLT = 4,  /* constant-voltage taper */
    PMIC_CHG_DONE       = 5,  /* full */
    PMIC_CHG_NA         = 6,  /* not applicable / unknown */
} pmic_charge_state_t;

typedef struct {
    bool                 ready;            /* false until first valid read */
    bool                 vbus_in;          /* USB-C connected */
    bool                 battery;          /* battery detected */
    pmic_charge_state_t  charge;
    int                  percent;          /* 0..100, or -1 if no battery */
    int                  voltage_mv;       /* battery voltage, 0 if no battery */
    int                  vbus_voltage_mv;  /* VBUS (USB) voltage, 0 if no VBUS */
    float                tdie_c;           /* PMIC die temperature, Celsius */
    float                rate_pct_per_min; /* +ve charging, -ve discharging,
                                            * derived from rolling samples.
                                            * Updates coarsely (the fuel gauge
                                            * reports integer percent only —
                                            * may stay 0 for minutes between
                                            * % steps). Pair with the mV rate
                                            * for a finer-grained read. */
    float                rate_mv_per_min;  /* battery voltage trend, smoother
                                            * proxy for actual charging rate
                                            * when the % field is sticky. */
} pmic_state_t;

bool pmic_init(void);
bool pmic_is_ready(void);
void pmic_get(pmic_state_t *out);

/* Human-readable label, e.g. "fast", "done", "off". Static storage. */
const char *pmic_charge_label(pmic_charge_state_t s);

#ifdef __cplusplus
}
#endif
