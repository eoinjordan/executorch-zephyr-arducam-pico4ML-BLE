/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 *
 * ICM-20948 on the Pico4ML-BLE: I2C0 (GP4 SDA, GP5 SCL), address 0x68,
 * shared with the camera's SCCB. Zephyr v4.4 has no driver for this part, so
 * this is the minimum: wake it, read accel + gyro from bank 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct imu_sample {
	float ax, ay, az; /* g */
	float gx, gy, gz; /* deg/s */
};

/* Returns 0 if WHO_AM_I answered 0xEA and the part is awake. */
int imu_init(void);
int imu_read(struct imu_sample *out);

#ifdef __cplusplus
}
#endif
