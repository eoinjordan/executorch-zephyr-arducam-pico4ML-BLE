/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 */
#include "camera.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>

/* pico-sdk defines pio0 as a macro, which would break DT_NODELABEL(pio0). */
#undef pio0
#undef pio1

#define HM01B0_ADDR 0x24

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const piodev = DEVICE_DT_GET(DT_NODELABEL(pio0));
static const struct pwm_dt_spec xclk = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct gpio_dt_spec reset_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), cam_reset_gpios);
static const struct gpio_dt_spec vsync_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), cam_vsync_gpios);
#define PIN_D0    DT_GPIO_PIN(DT_PATH(zephyr_user), cam_d0_gpios)
#define PIN_PCLK  DT_GPIO_PIN(DT_PATH(zephyr_user), cam_pclk_gpios)
#define PIN_HSYNC DT_GPIO_PIN(DT_PATH(zephyr_user), cam_hsync_gpios)

BUILD_ASSERT(PIN_PCLK == PIN_D0 + 8 && PIN_HSYNC == PIN_D0 + 9,
	     "PIO program waits on in-pin 8 (PCLK) and 9 (HSYNC) relative to D0");

static uint8_t frame[CAM_W * CAM_H];
static PIO pio;
static size_t sm;
static uint prog_offset;
static int dma_ch = -1;

struct reg {
	uint16_t addr;
	uint8_t val;
};

/* 324x244 QVGA window, 1-bit serial output, 8-bit per pixel; register
 * sequence after Arducam's RPI-Pico-Cam hm01b0_init.h. */
static const struct reg hm01b0_init[] = {
	{0x0103, 0x00}, {0x0100, 0x00}, {0x1003, 0x08}, {0x1007, 0x08}, {0x3044, 0x0A},
	{0x3045, 0x00}, {0x3047, 0x0A}, {0x3050, 0xC0}, {0x3051, 0x42}, {0x3052, 0x50},
	{0x3053, 0x00}, {0x3054, 0x03}, {0x3055, 0xF7}, {0x3056, 0xF8}, {0x3057, 0x29},
	{0x3058, 0x1F}, {0x3059, 0x1E}, {0x3064, 0x00}, {0x3065, 0x04}, {0x1000, 0x43},
	{0x1001, 0x40}, {0x1002, 0x32}, {0x0350, 0x7F}, {0x1006, 0x01}, {0x1008, 0x00},
	{0x1009, 0xA0}, {0x100A, 0x60}, {0x100B, 0x90}, {0x100C, 0x40}, {0x3022, 0x01},
	{0x1012, 0x01}, {0x2000, 0x07}, {0x2003, 0x00}, {0x2004, 0x1C}, {0x2007, 0x00},
	{0x2008, 0x58}, {0x200B, 0x00}, {0x200C, 0x7A}, {0x200F, 0x00}, {0x2010, 0xB8},
	{0x2013, 0x00}, {0x2014, 0x58}, {0x2017, 0x00}, {0x2018, 0x9B}, {0x2100, 0x01},
	{0x2101, 0x5F}, {0x2102, 0x0A}, {0x2103, 0x03}, {0x2104, 0x05}, {0x2105, 0x02},
	{0x2106, 0x14}, {0x2107, 0x02}, {0x2108, 0x03}, {0x2109, 0x03}, {0x210A, 0x00},
	{0x210B, 0x80}, {0x210C, 0x40}, {0x210D, 0x20}, {0x210E, 0x03}, {0x210F, 0x00},
	{0x2110, 0x85}, {0x2111, 0x00}, {0x2112, 0xA0}, {0x2150, 0x03}, {0x0340, 0x01},
	{0x0341, 0x7A}, {0x0342, 0x01}, {0x0343, 0x77}, {0x3010, 0x00}, {0x0383, 0x01},
	{0x0387, 0x01}, {0x0390, 0x00}, {0x3011, 0x70}, {0x3059, 0x22}, {0x3060, 0x30},
	{0x0101, 0x01}, {0x0104, 0x01},
#ifdef CONFIG_APP_CAM_BINNING
	/* 2x2 binning: 162x122 frames, a quarter of the RAM and four times the rate. */
	{0x0383, 0x03}, {0x0387, 0x03}, {0x0390, 0x03},
#endif
	{0x0100, 0x01},
};

static int reg_write(uint16_t addr, uint8_t val)
{
	uint8_t buf[3] = {(uint8_t)(addr >> 8), (uint8_t)addr, val};
	return i2c_write(i2c, buf, sizeof(buf), HM01B0_ADDR);
}

static int reg_read(uint16_t addr, uint8_t *val)
{
	uint8_t a[2] = {(uint8_t)(addr >> 8), (uint8_t)addr};
	return i2c_write_read(i2c, HM01B0_ADDR, a, sizeof(a), val, 1);
}

int camera_init(void)
{
	if (!device_is_ready(i2c) || !device_is_ready(piodev) || !pwm_is_ready_dt(&xclk) ||
	    !gpio_is_ready_dt(&reset_pin) || !gpio_is_ready_dt(&vsync_pin)) {
		printk("camera: a device is not ready\n");
		return -ENODEV;
	}

	/* XCLK: 125 MHz / 10 = 12.5 MHz, 30 % duty, like Arducam's PWM setup. */
	int ret = pwm_set_cycles(xclk.dev, xclk.channel, 10, 3, xclk.flags);
	if (ret != 0) {
		printk("camera: xclk pwm %d\n", ret);
		return ret;
	}

	gpio_pin_configure_dt(&reset_pin, GPIO_OUTPUT_ACTIVE); /* active low: held in reset */
	gpio_pin_configure_dt(&vsync_pin, GPIO_INPUT);
	k_msleep(100);
	gpio_pin_set_dt(&reset_pin, 0);
	k_msleep(100);

	uint8_t id[2] = {0, 0};
	ret = reg_read(0x0000, &id[0]);
	if (ret == 0) {
		ret = reg_read(0x0001, &id[1]);
	}
	if (ret != 0 || id[0] != 0x01 || id[1] != 0xB0) {
		printk("camera: HM01B0 not found (i2c %d, id %02x%02x)\n", ret, id[0], id[1]);
		return ret != 0 ? ret : -ENODEV;
	}
	for (size_t i = 0; i < ARRAY_SIZE(hm01b0_init); i++) {
		ret = reg_write(hm01b0_init[i].addr, hm01b0_init[i].val);
		if (ret != 0) {
			printk("camera: reg 0x%04x write failed (%d)\n", hm01b0_init[i].addr, ret);
			return ret;
		}
	}

	/* PIO: wait 1 pin 9 (HSYNC); wait 1 pin 8 (PCLK); in pins, 1; wait 0 pin 8. */
	static uint16_t instr[4];
	instr[0] = pio_encode_wait_pin(true, 9);
	instr[1] = pio_encode_wait_pin(true, 8);
	instr[2] = pio_encode_in(pio_pins, 1);
	instr[3] = pio_encode_wait_pin(false, 8);
	static const struct pio_program prog = {.instructions = instr, .length = 4, .origin = -1};

	pio = pio_rpi_pico_get_pio(piodev);
	ret = pio_rpi_pico_allocate_sm(piodev, &sm);
	if (ret != 0 || !pio_can_add_program(pio, &prog)) {
		printk("camera: no PIO state machine / program space\n");
		return -EBUSY;
	}
	uint offset = pio_add_program(pio, &prog);
	prog_offset = offset;
	pio_gpio_init(pio, PIN_D0);
	pio_gpio_init(pio, PIN_PCLK);
	pio_gpio_init(pio, PIN_HSYNC);
	pio_sm_set_consecutive_pindirs(pio, sm, PIN_D0, 1, false);
	pio_sm_set_consecutive_pindirs(pio, sm, PIN_PCLK, 2, false);

	pio_sm_config c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset, offset + 3);
	sm_config_set_in_pins(&c, PIN_D0);
	sm_config_set_in_shift(&c, false, true, 8); /* shift left, autopush every 8 bits */
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
	pio_sm_init(pio, sm, offset, &c);

	dma_ch = dma_claim_unused_channel(false);
	if (dma_ch < 0) {
		printk("camera: no free DMA channel\n");
		return -EBUSY;
	}
	printk("camera: HM01B0 ready, %ux%u, PIO sm %u, DMA ch %d\n", CAM_W, CAM_H, (unsigned)sm, dma_ch);
	return 0;
}

static bool wait_vsync(int level)
{
	int64_t deadline = k_uptime_get() + 1000;
	while (gpio_pin_get_dt(&vsync_pin) != level) {
		if (k_uptime_get() > deadline) {
			return false;
		}
	}
	return true;
}

const uint8_t *camera_capture(void)
{
	if (dma_ch < 0) {
		return NULL;
	}
	dma_channel_config c = dma_channel_get_default_config(dma_ch);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	dma_channel_configure(dma_ch, &c, frame, &pio->rxf[sm], sizeof(frame), false);

	/* Start on the rising edge of VSYNC so the DMA sees a whole frame. */
	if (!wait_vsync(0) || !wait_vsync(1)) {
		printk("camera: no VSYNC edge (pin stuck at %d); sensor not streaming\n",
		       gpio_pin_get_dt(&vsync_pin));
		return NULL;
	}
	pio_sm_clear_fifos(pio, sm);
	pio_sm_restart(pio, sm);
	pio_sm_exec(pio, sm, pio_encode_jmp(prog_offset));
	dma_channel_start(dma_ch);
	pio_sm_set_enabled(pio, sm, true);

	int64_t deadline = k_uptime_get() + 1000;
	while (dma_channel_is_busy(dma_ch)) {
		if (k_uptime_get() > deadline) {
			printk("camera: DMA stalled with %u bytes left (PCLK/HSYNC not toggling?)\n",
			       (unsigned)dma_channel_hw_addr(dma_ch)->transfer_count);
			dma_channel_abort(dma_ch);
			pio_sm_set_enabled(pio, sm, false);
			return NULL;
		}
		k_yield();
	}
	pio_sm_set_enabled(pio, sm, false);

#ifdef CONFIG_APP_CAM_BIT_REVERSE
	/* For sensor revisions that send the low bit first (not this one). */
	static uint8_t rev[256];
	if (rev[1] == 0) {
		for (unsigned v = 0; v < 256; v++) {
			unsigned r = 0;
			for (int b = 0; b < 8; b++) {
				r |= ((v >> b) & 1u) << (7 - b);
			}
			rev[v] = (uint8_t)r;
		}
	}
	for (size_t i = 0; i < sizeof(frame); i++) {
		frame[i] = rev[frame[i]];
	}
#endif
	return frame;
}

void camera_stats(const uint8_t *f, uint8_t *min, uint8_t *max, uint8_t *mean)
{
	uint32_t sum = 0;
	uint8_t lo = 255, hi = 0;
	for (size_t i = 0; i < (size_t)CAM_W * CAM_H; i++) {
		uint8_t v = f[i];
		sum += v;
		lo = v < lo ? v : lo;
		hi = v > hi ? v : hi;
	}
	*min = lo;
	*max = hi;
	*mean = (uint8_t)(sum / ((size_t)CAM_W * CAM_H));
}
