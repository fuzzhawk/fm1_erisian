/* SPDX-License-Identifier: MIT */
#ifndef ERISIAN_TABLES_H
#define ERISIAN_TABLES_H
#include <stdint.h>
#include "fm1.h" /* FM1_AUDIO_DATA */
/* Not const: they live in the RAM section the audio interrupt reads. */
extern int16_t table_sine[257];
extern uint32_t table_tune[128];
extern uint32_t table_decay[128];
extern uint16_t table_ratio[16];
#endif
