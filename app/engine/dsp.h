/* SPDX-License-Identifier: MIT */
/* Small fixed-point helpers shared by the drum and synth engines. All run in
 * the audio interrupt (RAM, no libc, no division); each file that includes
 * this gets its own copy in RAM. */
#ifndef ERISIAN_DSP_H
#define ERISIAN_DSP_H

#include <stdint.h>

#include "fm1.h"
#include "tables.h"

FM1_AUDIO_CODE static inline int32_t sine(uint32_t phase)
{
    uint32_t i = phase >> 24, frac = (phase >> 8) & 0xFFFFu;
    int32_t a = table_sine[i], b = table_sine[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)frac) >> 16);
}

FM1_AUDIO_CODE static inline uint32_t decay(uint32_t env, uint32_t coef)
{
    return (uint32_t)(((uint64_t)env * coef) >> 30);
}

FM1_AUDIO_CODE static inline uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

FM1_AUDIO_CODE static inline int32_t clamp15(int32_t y)
{
    return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

/* Chamberlin state-variable filter, two passes per sample (2x), f and q Q14.
 * Returns low, band or high per type (1, 3, 2). */
FM1_AUDIO_CODE static inline int32_t svf2x(int32_t *low, int32_t *band, int32_t x,
                                           int32_t f, int32_t q, unsigned type)
{
    int32_t high = 0;
    for (unsigned i = 0; i < 2; i++) {
        *low += (f * *band) >> 14;
        high = x - *low - ((q * *band) >> 14);
        *band += (f * high) >> 14;
        if (*band > (1 << 20)) *band = 1 << 20;
        if (*band < -(1 << 20)) *band = -(1 << 20);
        if (*low > (1 << 20)) *low = 1 << 20;
        if (*low < -(1 << 20)) *low = -(1 << 20);
    }
    return type == 1 ? *low : type == 2 ? high : *band;
}

/* Soft clip in Q15: cubic on [-1.5, 1.5]. */
FM1_AUDIO_CODE static inline int32_t soft15(int32_t x)
{
    if (x > 49152) x = 49152;
    if (x < -49152) x = -49152;
    int32_t x3 = (int32_t)((((int64_t)x * x >> 15) * x) >> 15);
    return clamp15(x - ((x3 * 9709) >> 16));      /* x - 4/27 x^3 */
}

FM1_AUDIO_CODE static inline int32_t white(uint32_t *s)
{
    return (int32_t)(int16_t)(rnd(s) >> 16);
}

#endif
