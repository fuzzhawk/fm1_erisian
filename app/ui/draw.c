/* SPDX-License-Identifier: MIT */
#include "draw.h"

#include <string.h>

#include "fm1.h"
#include "font5x7.h"

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
