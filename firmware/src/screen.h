/* Copyright (c) 2026 Eoin Jordan
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Pico4ML's 0.96" ST7735S (80 x 160, portrait) through Zephyr's display
 * API. One RGB565 framebuffer in RAM, pushed with display_write().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_W 80
#define SCREEN_H 160

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BLACK  RGB565(0, 0, 0)
#define COLOR_WHITE  RGB565(255, 255, 255)
#define COLOR_GREEN  RGB565(40, 220, 90)
#define COLOR_RED    RGB565(240, 60, 60)
#define COLOR_YELLOW RGB565(240, 200, 60)
#define COLOR_GREY   RGB565(110, 110, 120)

int screen_init(void);
void screen_clear(uint16_t color);
void screen_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* 8-bit grayscale image, drawn with integer scale (28x28 at scale 2 = 56x56). */
void screen_gray(int x, int y, const uint8_t *img, int w, int h, int scale);
void screen_bar(int x, int y, int w, int h, int percent, uint16_t color);
/* Push the framebuffer to the panel. */
void screen_flush(void);

#ifdef __cplusplus
}
#endif
