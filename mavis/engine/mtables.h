/* SPDX-License-Identifier: MIT */
#ifndef MAVIS_MTABLES_H
#define MAVIS_MTABLES_H
#include <stdint.h>
#include "fm1.h" /* FM1_AUDIO_DATA */
/* Not const: they live in the RAM section the audio interrupt reads. */
extern int16_t mt_sine[1025];          /* Q15, 1024 per cycle */
extern uint32_t mt_note_inc[128];      /* MIDI note -> phase increment */
extern uint32_t mt_note_period[128];   /* MIDI note -> period, samples Q16 */
extern uint32_t mt_semi_up[257];       /* 2^(i/3072), Q16 */
extern uint32_t mt_semi_down[257];     /* 2^(-i/3072), Q16 */
extern uint32_t mt_exp2_neg[257];      /* 2^(-i/256), Q16 */
#endif
