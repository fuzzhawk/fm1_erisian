/* SPDX-License-Identifier: MIT */
#include "mix.h"

#include "fm1.h"

/* y = x - (4/27) x^3 on x in [-1.5, 1.5] maps 1.5 to 1.0 with zero slope
 * there; nearly linear below ~0.3 (error < 1.5 %). x in Q23 units of full
 * scale. */
FM1_AUDIO_CODE void mix_master(int32_t *buf, unsigned frames, int32_t gain_q15)
{
    const int64_t fs = 1 << 23;
    const int64_t limit = fs + fs / 2; /* 1.5 */
    for (unsigned i = 0; i < 2 * frames; i++) {
        int64_t x = ((int64_t)buf[i] * gain_q15) >> 15;
        if (x > limit) x = limit;
        if (x < -limit) x = -limit;
        int64_t x3 = (((x * x) >> 23) * x) >> 23;          /* x^3, Q23 */
        int64_t y = x - ((x3 * 9709) >> 16);                /* 4/27 = 9709/65536 */
        if (y > MIX_FULL_SCALE) y = MIX_FULL_SCALE;
        if (y < -MIX_FULL_SCALE) y = -MIX_FULL_SCALE;
        buf[i] = (int32_t)y;
    }
}
