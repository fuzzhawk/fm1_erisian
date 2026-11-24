/* SPDX-License-Identifier: MIT */
#include "draw.h"

#include <string.h>

#include "fm1.h"
#include "ui/font5x7.h"

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    const char *p = strchr(font5x7_chars, c);
    if (!p || !c)
        p = strchr(font5x7_chars, '?');
    return font5x7_rows[p - font5x7_chars];
}

unsigned draw_text(unsigned x, unsigned y, const char *text, unsigned scale,
                   uint16_t fg, uint16_t bg)
{
    static uint16_t cell[6 * 3 * 8 * 3];
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    const unsigned w = 6 * scale, h = 8 * scale;
    for (; *text; text++) {
        if (x + w > FM1_SCREEN_WIDTH || y + h > FM1_SCREEN_HEIGHT)
            break;
        const uint8_t *rows = glyph(*text);
        for (unsigned py = 0; py < h; py++) {
            unsigned row = py / scale;
            uint8_t bits = row < 7 ? rows[row] : 0;
            for (unsigned px = 0; px < w; px++) {
                unsigned col = px / scale;
                int on = col < 5 && (bits >> (4 - col)) & 1;
                cell[py * w + px] = on ? fg : bg;
            }
        }
        fm1_screen_blit(x, y, w, h, cell, w * h);
        x += w;
    }
    return x;
}

void draw_line(unsigned x, unsigned y, const char *text, unsigned scale,
               uint16_t fg, uint16_t bg)
{
    unsigned end = draw_text(x, y, text, scale, fg, bg);
    if (end < FM1_SCREEN_WIDTH)
        fm1_screen_fill(end, y, FM1_SCREEN_WIDTH - end, 8 * scale, bg);
}

char *fmt_uint(char *out, uint32_t value)
{
    char tmp[11];
    unsigned n = 0;
    do { tmp[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = tmp[--n];
    *out = 0;
    return out;
}

char *fmt_hex(char *out, uint32_t value, unsigned digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = digits; i--;)
        *out++ = hex[(value >> (4 * i)) & 15];
    *out = 0;
    return out;
}

char *fmt_str(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    *out = 0;
    return out;
}

/* Text centred in a field w pixels wide and 16 high: scale 2 when it fits
 * (letters 1 px closer for 5 characters, so neighbouring columns keep a gap),
 * else scale 1. The whole field is painted, so no clear (and no flicker). */
void ui_field(unsigned x, unsigned y, unsigned w, const char *text, uint16_t fg)
{
    unsigned len = (unsigned)strlen(text), scale = 2, pitch = 12;
    if (len * 12u > w - 4u) pitch = 11;              /* 5 letters in 60 px */
    if (len && (len - 1u) * pitch + 12u > w - 2u) { scale = 1; pitch = 6; }
    if (len && (len - 1u) * pitch + 6u * scale > w) len = (w - 6u * scale) / pitch + 1u;
    unsigned total = len ? (len - 1u) * pitch + 6u * scale : 0;
    unsigned x0 = x + (w - total) / 2u, ty = scale == 1 ? y + 4u : y;
    char one[2] = { 0, 0 };
    if (x0 > x) fm1_screen_fill(x, y, x0 - x, 16, COLOR_BG);
    if (scale == 1 && total) {
        fm1_screen_fill(x0, y, total, 4, COLOR_BG);
        fm1_screen_fill(x0, y + 12, total, 4, COLOR_BG);
    }
    for (unsigned i = 0; i < len; i++) {
        one[0] = text[i];
        draw_text(x0 + i * pitch, ty, one, scale, fg, COLOR_BG);
    }
    if (x0 + total < x + w) fm1_screen_fill(x0 + total, y, x + w - x0 - total, 16, COLOR_BG);
}

/* A value bar h pixels tall: the coloured span grows from the bottom, or from
 * the middle when bipolar. max 0 = nothing to show (blank). */
void ui_bar(unsigned x, unsigned y, unsigned w, unsigned h, unsigned value, unsigned max,
            int bipolar, uint16_t color)
{
    unsigned a = h, b = h;          /* coloured span [a, b) from the top */
    if (max) {
        if (value > max) value = max;
        unsigned level = h - value * h / max;
        if (bipolar) {
            unsigned mid = h / 2u;
            a = level < mid ? level : mid;
            b = level < mid ? mid : level;
            if (a == b) { a = mid - 1u; b = mid + 1u; }
        } else {
            a = level;
            if (a >= h) a = h - 1u;       /* a sliver at zero: the slot exists */
        }
    }
    uint16_t off = max ? UI_BAR_OFF : COLOR_BG;
    if (a) fm1_screen_fill(x, y, w, a, off);
    if (b > a) fm1_screen_fill(x, y + a, w, b - a, color);
    if (b < h) fm1_screen_fill(x, y + b, w, h - b, off);
}
