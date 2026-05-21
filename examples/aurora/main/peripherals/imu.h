/*
 * imu.h — QMI8658C accelerometer.
 *
 * On the Waveshare ESP32-S3-Touch-AMOLED-2.16, the IMU sits on the SAME
 * I²C bus as the CST9217 touch (SDA=15 SCL=14). The BSP exposes the bus
 * handle via `bsp_i2c_get_handle()`; this module attaches a device on
 * that bus rather than creating a new one.
 *
 * Threading: a background task polls the chip at ~100 Hz and applies an
 * exponential moving average for tilt smoothing. Foreground readers (the
 * Tilt scene, the `?sensor` console command) call the lock-free getter
 * which returns the latest filtered values.
 *
 * Returned values are deltas (in g) from the neutral pose captured at
 * boot. Hold the board still during the first ~250 ms of imu_init.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool imu_init(void);
bool imu_is_ready(void);

/* Accel — delta from neutral pose, in g. */
void imu_get_accel(float *ax, float *ay, float *az);

/* Gyroscope — raw angular rate in degrees per second.
 * QMI8658 default config (set by Waveshare's qmi8658_init) is 512 dps
 * range at 1 kHz ODR; the background task averages every chunk and
 * gates with an EMA so static-board readings sit near 0. */
void imu_get_gyro(float *gx, float *gy, float *gz);

/* Internal die temperature in Celsius. The QMI8658 has a built-in
 * temperature ADC; we read its raw 16-bit register and convert with
 * the manufacturer formula (T = raw / 256 + offset). Useful for the
 * System scene as an "MCU-area" thermal proxy alongside the AXP2101's
 * TDIE channel. */
float imu_get_temp_c(void);

#ifdef __cplusplus
}
#endif
