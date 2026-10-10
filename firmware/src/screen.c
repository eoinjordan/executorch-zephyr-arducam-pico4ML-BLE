/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 */
#include "screen.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "font6x8.h"

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static uint16_t fb[SCREEN_W * SCREEN_H];
static bool ready;
static bool swap_bytes;

/* The panel wants the high byte first on the wire. With PIXEL_FORMAT_RGB_565X
 * (what the st7735r driver reports) the buffer holds big-endian RGB565, so
 * swap here; with RGB_565 the driver does it. */
static inline uint16_t pack(uint16_t c)
{
	return swap_bytes ? (uint16_t)((c << 8) | (c >> 8)) : c;
}

static inline void put(int x, int y, uint16_t c)
{
	if (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H) {
		fb[y * SCREEN_W + x] = pack(c);
	}
}

int screen_init(void)
{
	if (!device_is_ready(disp)) {
		printk("screen: display not ready\n");
		return -ENODEV;
	}
	struct display_capabilities caps;
	display_get_capabilities(disp, &caps);
	printk("screen: %ux%u, pixel format 0x%x\n", caps.x_resolution, caps.y_resolution,
	       caps.current_pixel_format);
	if (caps.current_pixel_format == PIXEL_FORMAT_RGB_565X) {
		swap_bytes = true;
	} else if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
		int ret = display_set_pixel_format(disp, PIXEL_FORMAT_RGB_565);
		if (ret != 0) {
			printk("screen: no 16-bit colour mode (%d)\n", ret);
			return ret;
		}
	}
	ready = true;
	screen_clear(COLOR_BLACK);
	screen_flush();
	display_blanking_off(disp);
	return 0;
}

void screen_clear(uint16_t color)
{
	uint16_t c = pack(color);
	for (size_t i = 0; i < SCREEN_W * SCREEN_H; i++) {
		fb[i] = c;
	}
}

void screen_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
	for (; *s; s++, x += FONT_W) {
		unsigned c = (unsigned char)*s;
		if (c < FONT_FIRST || c > FONT_LAST) {
			c = '?';
		}
		const uint8_t *g = kFont6x8[c - FONT_FIRST];
		for (int cx = 0; cx < FONT_W; cx++) {
			for (int cy = 0; cy < FONT_H; cy++) {
				put(x + cx, y + cy, (g[cx] >> cy) & 1 ? fg : bg);
			}
		}
	}
}

void screen_gray(int x, int y, const uint8_t *img, int w, int h, int scale)
{
	for (int iy = 0; iy < h; iy++) {
		for (int ix = 0; ix < w; ix++) {
			uint8_t v = img[iy * w + ix];
			uint16_t c = RGB565(v, v, v);
			for (int sy = 0; sy < scale; sy++) {
				for (int sx = 0; sx < scale; sx++) {
					put(x + ix * scale + sx, y + iy * scale + sy, c);
				}
			}
		}
	}
}

void screen_bar(int x, int y, int w, int h, int percent, uint16_t color)
{
	if (percent < 0) {
		percent = 0;
	} else if (percent > 100) {
		percent = 100;
	}
	int fill = w * percent / 100;
	for (int iy = 0; iy < h; iy++) {
		for (int ix = 0; ix < w; ix++) {
			put(x + ix, y + iy, ix < fill ? color : COLOR_GREY);
		}
	}
}

void screen_flush(void)
{
	if (!ready) {
		return;
	}
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(fb),
		.width = SCREEN_W,
		.height = SCREEN_H,
		.pitch = SCREEN_W,
	};
	int ret = display_write(disp, 0, 0, &desc, fb);
	if (ret != 0) {
		printk("screen: display_write %d\n", ret);
	}
}
