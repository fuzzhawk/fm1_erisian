/* SPDX-License-Identifier: MIT */
/* ERISIAN drum sequencer core: 11 voices x 64 steps, sample-accurate.
 *
 * Time runs in pulses: 12 per 1/16 step at 1x speed, so every per-voice speed
 * (2x, 1x, 3/4x, 1/2x, 1/4x) and ratchet count (1-4) lands on whole pulses.
 * The master length (1-64 steps of 1x) sets when the pattern ends; at that
 * point every voice restarts from its first step. Each voice also loops its
 * own length inside the master length (polyrhythms).
 *
 * Micro-timing moves a step by twelfths of the voice's step: late steps wait
 * like swing does, early steps fire from the end of the step before (on time
 * when there is no step before, e.g. right after PLAY).
 *
 * seq_render() is called once per audio block and reports trigger events with
 * their frame offset inside the block, so timing does not depend on the block
 * size. It does no allocation, I/O or float maths: safe for the audio
 * interrupt.
 */
#ifndef ERISIAN_SEQ_H
#define ERISIAN_SEQ_H

#include <stdint.h>

#define SEQ_VOICES 11u
#define SEQ_STEPS 64u
#define SEQ_PULSES_PER_STEP 12u
#define SEQ_RATE 44100u
#define SEQ_BPM_MIN 3000u      /* tempo in 1/100 BPM: 30.00 */
#define SEQ_BPM_MAX 30000u     /* 300.00 */

enum seq_speed { SEQ_SPEED_2X, SEQ_SPEED_1X, SEQ_SPEED_3_4X, SEQ_SPEED_1_2X,
                 SEQ_SPEED_1_4X, SEQ_SPEED_COUNT };

#define SEQ_MICRO_MAX 6     /* micro-timing: -6..+6 twelfths of a step */
#define SEQ_LOCK_SLOTS 64u  /* parameter-locked steps per pattern */
#define SEQ_LOCK_VALUES 32u /* lockable parameters per step (see plock.h) */
#define SEQ_UNLOCKED 0xFFu

struct seq_step {
    uint8_t on;
    uint8_t velocity;      /* 1..127 */
    uint8_t probability;   /* 0..100 %, 100 = always */
    uint8_t ratchet;       /* 1..4 hits in the step */
    int8_t micro;          /* -6..+6: early/late by twelfths of the step */
    uint8_t lock;          /* 0 = none, else parameter-lock slot + 1 */
};

/* One parameter-locked step: which step it belongs to and the locked values
 * (SEQ_UNLOCKED = follows the kit). The sequencer only passes the slot on;
 * the drum engine plays the step with these values (app/engine/plock.c). */
struct seq_lock {
    uint8_t voice;         /* SEQ_UNLOCKED = slot free */
    uint8_t step;
    uint8_t value[SEQ_LOCK_VALUES];
};

/* Layout saved by project formats 1-3 (before micro-timing and locks). */
struct seq_step_v3 { uint8_t on, velocity, probability, ratchet; };

struct seq_voice_track {
    struct seq_step steps[SEQ_STEPS];
    uint8_t length;        /* 1..64 steps */
    uint8_t speed;         /* enum seq_speed */
    uint8_t swing;         /* 50..75 %: 50 = straight */
    uint8_t mute;
};

struct seq_pattern {
    struct seq_voice_track voices[SEQ_VOICES];
    uint8_t length;        /* master length, 1..64 steps of 1x */
    uint16_t bpm100;
    struct seq_lock locks[SEQ_LOCK_SLOTS];
};

struct seq_voice_track_v3 {
    struct seq_step_v3 steps[SEQ_STEPS];
    uint8_t length, speed, swing, mute;
};
struct seq_pattern_v3 {
    struct seq_voice_track_v3 voices[SEQ_VOICES];
    uint8_t length;
    uint16_t bpm100;
};
void seq_pattern_from_v3(struct seq_pattern *pattern, const struct seq_pattern_v3 *old);

struct seq_event {
    uint16_t frame;        /* offset in the block */
    uint8_t voice;
    uint8_t velocity;
    uint8_t lock;          /* 0 = none, else lock slot + 1 */
};

struct seq_state {
    uint32_t pulse;        /* 0 .. length * 12 - 1 */
    uint32_t phase;        /* sub-pulse accumulator */
    uint32_t random;
    uint8_t playing;
    uint8_t vpulse[SEQ_VOICES];          /* pulse inside the voice's step */
    uint8_t vindex[SEQ_VOICES];          /* the voice's current step */
    /* A swung or ratcheted hit waits here for its pulse (one per voice). */
    uint8_t pending_left[SEQ_VOICES];   /* hits still to play in this step */
    uint8_t pending_gap[SEQ_VOICES];    /* pulses between ratchet hits */
    uint8_t pending_wait[SEQ_VOICES];   /* pulses until the next hit */
    uint8_t pending_velocity[SEQ_VOICES];
    uint8_t pending_lock[SEQ_VOICES];
    /* Early (negative micro-timing) step already played, 0xFF none. */
    uint8_t early_step[SEQ_VOICES];
};

void seq_pattern_init(struct seq_pattern *pattern);
void seq_start(struct seq_state *state, uint32_t seed);   /* from step 1 */
void seq_stop(struct seq_state *state);

/* Pulses per step for a speed (6, 12, 16, 24, 48). */
unsigned seq_speed_period(unsigned speed);

/* Advance by frames; writes up to max_events events, returns how many. */
unsigned seq_render(struct seq_state *state, const struct seq_pattern *pattern,
                    unsigned frames, struct seq_event *events, unsigned max_events);

/* Current 1x step (0..63) for the playhead display. */
static inline unsigned seq_master_step(const struct seq_state *s)
{
    return s->pulse / SEQ_PULSES_PER_STEP;
}

/* Step index a voice is on at the current pulse. */
unsigned seq_voice_step(const struct seq_state *state,
                        const struct seq_pattern *pattern, unsigned voice);

#endif
