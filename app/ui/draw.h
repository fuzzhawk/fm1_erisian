/* SPDX-License-Identifier: MIT */
/* Text drawing on the 240x240 RGB565 panel. */
#ifndef ERISIAN_DRAW_H
#define ERISIAN_DRAW_H
#include <stdint.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COLOR_BG     RGB565(0, 0, 0)
#define COLOR_TEXT   RGB565(220, 220, 220)
#define COLOR_DIM    RGB565(110, 110, 110)
#define COLOR_GOLD   RGB565(255, 196, 0)
#define COLOR_CYAN   RGB565(0, 220, 255)
#define COLOR_RED    RGB565(255, 60, 40)

/* Glyph cell: 6x8 pixels times scale (1..3). Unknown characters draw as '?'.
 * Lower-case letters draw as upper case. Returns the x after the text. */
unsigned draw_text(unsigned x, unsigned y, const char *text, unsigned scale,
                   uint16_t fg, uint16_t bg);

/* Draw text and clear the rest of the line to the right edge. */
void draw_line(unsigned x, unsigned y, const char *text, unsigned scale,
               uint16_t fg, uint16_t bg);

/* Small formatting helpers (no printf in the control task hot path). */
char *fmt_uint(char *out, uint32_t value);
char *fmt_hex(char *out, uint32_t value, unsigned digits);
char *fmt_str(char *out, const char *text);

#endif
