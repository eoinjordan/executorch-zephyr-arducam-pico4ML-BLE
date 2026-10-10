/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 *
 * HM01B0 on the Pico4ML, captured through PIO + DMA the way Arducam's
 * RPI-Pico-Cam does it: the sensor is set to its 1-bit serial output, one PIO
 * state machine clocks a bit in on every PCLK while HSYNC is high and pushes
 * a byte per pixel into its FIFO, and a DMA channel drains the FIFO into RAM.
 *
 * Pins (Pico4ML, from the devicetree zephyr,user node):
 *   SIOC GP5 / SIOD GP4   SCCB (I2C0), address 0x24
 *   RESET GP2             active low
 *   XCLK GP3              PWM, 12.5 MHz
 *   VSYNC GP16            GPIO input
 *   D0 GP6, PCLK GP14, HSYNC GP15   PIO inputs (PCLK/HSYNC must be D0+8/D0+9)
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_APP_CAM_BINNING
#define CAM_W 162
#define CAM_H 122
#else
#define CAM_W 324
#define CAM_H 244
#endif

/* Returns 0, or -errno if the sensor does not answer or PIO is unavailable. */
int camera_init(void);

/* Captures one grayscale frame into the internal buffer and returns it.
 * NULL on timeout (no VSYNC within a second). */
const uint8_t *camera_capture(void);

/* Raw frame statistics, for telling a dark scene from a dead capture. */
void camera_stats(const uint8_t *frame, uint8_t *min, uint8_t *max, uint8_t *mean);

#ifdef __cplusplus
}
#endif
