/* SPDX-License-Identifier: MIT */
/* Master stage: headroom gain, then a cubic soft clip to full scale.
 * Audio interrupt; integer maths. */
#ifndef ERISIAN_MIX_H
#define ERISIAN_MIX_H
#include <stdint.h>

#define MIX_FULL_SCALE ((1 << 23) - 1)  /* Q23 */

/* buf: interleaved stereo Q23 sums (may exceed full scale). gain Q15
 * (32768 = 1.0). Output never exceeds +-MIX_FULL_SCALE. */
void mix_master(int32_t *buf, unsigned frames, int32_t gain_q15);

#endif
