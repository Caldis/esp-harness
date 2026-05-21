/*
 * mock_peripherals.h — host stubs for everything in main/peripherals/.
 *
 * Each function returns a sensible default that lets a peripheral-using
 * scene compile and render *something* (neutral pose / 100% battery /
 * empty scan / no card). For full visual fidelity you'd want fake
 * dynamic data (e.g. simulated tilt from mouse), but the v1.1 scaffold
 * just gets it building.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "peripherals/imu.h"
#include "peripherals/pmic.h"
#include "peripherals/audio.h"
#include "peripherals/wifi.h"
#include "peripherals/ble.h"
#include "peripherals/sdcard.h"
#include "peripherals/keys.h"
#include "peripherals/system.h"
#include "peripherals/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side helpers — main loop calls these to push fake state. */
void mock_keys_set_boot(bool pressed);
void mock_keys_set_user(bool pressed);

#ifdef __cplusplus
}
#endif
