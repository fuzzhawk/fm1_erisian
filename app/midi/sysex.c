/* SPDX-License-Identifier: MIT */
#include "sysex.h"

size_t sysex_pack(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 7) {
        size_t group = n - i < 7 ? n - i : 7;
        uint8_t high = 0;
        for (size_t k = 0; k < group; k++)
            high |= (uint8_t)((in[i + k] >> 7) << (6 - k));
        out[o++] = high;
        for (size_t k = 0; k < group; k++)
            out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

size_t sysex_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t capacity)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 8) {
        size_t group = n - i - 1 < 7 ? n - i - 1 : 7;
        if (n - i < 2)
            return (size_t)-1;
        uint8_t high = in[i];
        if (high & 0x80)
            return (size_t)-1;
        for (size_t k = 0; k < group; k++) {
            uint8_t b = in[i + 1 + k];
            if ((b & 0x80) || o >= capacity)
                return (size_t)-1;
            out[o++] = (uint8_t)(b | (((high >> (6 - k)) & 1u) << 7));
        }
    }
    return o;
}
