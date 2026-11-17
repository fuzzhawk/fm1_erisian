/* SPDX-License-Identifier: MIT */
/* Parameter locks: a step can override any engine or common-chain parameter
 * of its voice. The pattern keeps a pool of SEQ_LOCK_SLOTS locked steps
 * (seq_pattern.locks); each is prepared into drums.locked[slot] so the audio
 * interrupt only switches a pointer when the step plays.
 *
 * Lock value index: 0..12 engine parameter e[i], 16..29 common c[j].
 * Control task only. Write order keeps the interrupt safe: a slot is filled
 * and prepared before a step points at it, and a step lets go before its
 * slot is freed.
 */
#ifndef ERISIAN_PLOCK_H
#define ERISIAN_PLOCK_H

#include "drums.h"
#include "seq.h"

#define PLOCK_E(i) (i)
#define PLOCK_C(j) (16u + (j))

/* Lock slot of a step, or -1. */
int plock_slot(const struct seq_pattern *p, unsigned voice, unsigned step);
/* The value a step plays for a lock index; *locked says if it is locked. */
unsigned plock_value(const struct seq_pattern *p, const struct drum_kit *kit,
                     unsigned voice, unsigned step, unsigned index, int *locked);
/* Lock a value on a step. 0, or -1 when the pool is full. */
int plock_set(struct seq_pattern *p, const struct drum_kit *kit, struct drums *d,
              unsigned voice, unsigned step, unsigned index, unsigned value);
/* Remove every lock of a step. */
void plock_clear_step(struct seq_pattern *p, unsigned voice, unsigned step);
/* The voice's engine changed: its engine-parameter locks mean nothing now. */
void plock_clear_engine(struct seq_pattern *p, const struct drum_kit *kit, struct drums *d,
                        unsigned voice);
/* Re-prepare a voice's locked steps (after a kit edit), all of them (after a
 * load), or those whose sound depends on tempo (METAL reverse, NOISE). */
void plock_prepare_voice(const struct seq_pattern *p, const struct drum_kit *kit,
                         struct drums *d, unsigned voice);
void plock_prepare_all(const struct seq_pattern *p, const struct drum_kit *kit, struct drums *d);
void plock_tempo(const struct seq_pattern *p, const struct drum_kit *kit, struct drums *d);
/* After loading: drop links that do not match in both directions. */
void plock_validate(struct seq_pattern *p);
unsigned plock_free_slots(const struct seq_pattern *p);

#endif
