/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 *
 * TDK IMU on the Pico4ML's I2C0 (GP4 SDA, GP5 SCL), address 0x68. The
 * product pages say ICM-20948; the board on my desk answers as an ICM-42602
 * (WHO_AM_I 0x41), so probe for both families and speak the matching map.
 */
#include "imu.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));

enum part { PART_NONE, PART_ICM20948, PART_ICM4262X };
static enum part part;
static uint8_t addr;

/* ICM-20948 (bank 0) */
#define ICM20948_WHO_AM_I   0x00 /* 0xEA */
#define ICM20948_PWR_MGMT_1 0x06
#define ICM20948_ACCEL_XOUT 0x2D
#define ICM20948_BANK_SEL   0x7F

/* ICM-42622 / 42605 family (bank 0) */
#define ICM4262X_WHO_AM_I   0x75 /* 0x46 for ICM-42622, 0x42 for ICM-42605 */
#define ICM4262X_PWR_MGMT0  0x4E
#define ICM4262X_ACCEL_X1   0x1F /* 6 bytes accel, 6 bytes gyro follow */

static int rd(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(bus, addr, reg, val);
}

static int wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(bus, addr, reg, val);
}

int imu_init(void)
{
	if (!device_is_ready(bus)) {
		return -ENODEV;
	}
	static const uint8_t addrs[] = {0x68, 0x69};
	for (size_t i = 0; i < ARRAY_SIZE(addrs) && part == PART_NONE; i++) {
		uint8_t who75 = 0xFF, who00 = 0xFF;
		addr = addrs[i];
		int r75 = rd(ICM4262X_WHO_AM_I, &who75);
		/* 0x41 ICM-42602 (what this Pico4ML-BLE has), 0x42 ICM-42605, 0x46 ICM-42622,
		 * 0x47 ICM-42688, 0x67 ICM-42670 */
		if (r75 == 0 && (who75 == 0x41 || who75 == 0x42 || who75 == 0x46 || who75 == 0x47 ||
				 who75 == 0x67)) {
			part = PART_ICM4262X;
			printk("imu: ICM-4262x family (WHO_AM_I 0x%02x) at 0x%02x\n", who75, addr);
			break;
		}
		int r00 = wr(ICM20948_BANK_SEL, 0x00);
		if (r00 == 0) {
			r00 = rd(ICM20948_WHO_AM_I, &who00);
		}
		if (r00 == 0 && who00 == 0xEA) {
			part = PART_ICM20948;
			printk("imu: ICM-20948 at 0x%02x\n", addr);
			break;
		}
		printk("imu: 0x%02x: reg 0x75 -> %d/0x%02x, reg 0x00 -> %d/0x%02x\n", addr, r75, who75,
		       r00, who00);
	}
	if (part == PART_NONE) {
		printk("imu: no known TDK IMU on i2c0\n");
		return -ENODEV;
	}
	if (part == PART_ICM20948) {
		wr(ICM20948_PWR_MGMT_1, 0x80); /* reset */
		k_msleep(20);
		wr(ICM20948_PWR_MGMT_1, 0x01); /* wake, auto clock */
	} else {
		wr(ICM4262X_PWR_MGMT0, 0x0F); /* accel + gyro in low-noise mode */
	}
	k_msleep(50);
	printk("imu: ready (accel +-%s, gyro +-%s)\n", part == PART_ICM20948 ? "2 g" : "16 g",
	       part == PART_ICM20948 ? "250 dps" : "2000 dps");
	return 0;
}

int imu_read(struct imu_sample *out)
{
	uint8_t raw[12];
	uint8_t reg = part == PART_ICM20948 ? ICM20948_ACCEL_XOUT : ICM4262X_ACCEL_X1;
	int ret = i2c_burst_read(bus, addr, reg, raw, sizeof(raw));
	if (ret != 0) {
		return ret;
	}
	int16_t v[6];
	for (int i = 0; i < 6; i++) {
		v[i] = (int16_t)sys_get_be16(&raw[2 * i]);
	}
	/* ICM-20948 defaults: +-2 g (16384 LSB/g), +-250 dps (131 LSB/dps).
	 * ICM-4262x defaults: +-16 g (2048 LSB/g), +-2000 dps (16.4 LSB/dps). */
	float acc_lsb = part == PART_ICM20948 ? 16384.f : 2048.f;
	float gyr_lsb = part == PART_ICM20948 ? 131.f : 16.4f;
	out->ax = v[0] / acc_lsb;
	out->ay = v[1] / acc_lsb;
	out->az = v[2] / acc_lsb;
	out->gx = v[3] / gyr_lsb;
	out->gy = v[4] / gyr_lsb;
	out->gz = v[5] / gyr_lsb;
	return 0;
}
