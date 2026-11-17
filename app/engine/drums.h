/* SPDX-License-Identifier: MIT */
/* ERISIAN drum voices: 11 voices, each one engine (Machinedrum style).
 * Engines in this first slice: SAMPLE (the uploaded kick) and FM (2-operator
 * FM with pitch sweep, plus filtered noise: kicks, snares, toms, hats).
 *
 * drums_prepare() turns the kit's 0..127 parameters into fixed-point
 * constants (control task: may divide and use tables freely).
 * drums_render() runs in the audio interrupt: integer maths, RAM only.
 */
#ifndef ERISIAN_DRUMS_H
#define ERISIAN_DRUMS_H

#include <stdint.h>

#include "seq.h"

#define DRUM_VOICES SEQ_VOICES
#define DRUM_CHOKE_GROUPS 4u

enum drum_engine { DRUM_OFF, DRUM_SAMPLE, DRUM_FM, DRUM_ENGINES };

enum drum_param {
    DP_LEVEL, DP_PAN, DP_TUNE, DP_DECAY,
    DP_PITCH_ENV, DP_PITCH_DECAY, DP_RATIO, DP_MOD, DP_MOD_DECAY, DP_FEEDBACK,
    DP_NOISE, DP_NOISE_DECAY, DP_NOISE_TONE,
    DP_COUNT
};

/* What a kit stores per voice (saved with patterns and kits). */
struct drum_voice_params {
    uint8_t engine;          /* enum drum_engine */
    uint8_t choke;           /* 0 = none, 1..4 */
    uint8_t p[DP_COUNT];     /* 0..127 each */
};

struct drum_kit {
    struct drum_voice_params voice[DRUM_VOICES];
};

/* Fixed-point form of one voice's parameters. */
struct drum_derived {
    uint8_t engine, choke;
    uint32_t base_inc;       /* carrier phase increment */
    uint32_t sweep;          /* extra increment at full pitch envelope */
    uint32_t amp_coef, pitch_coef, mod_coef, noise_coef; /* Q30 per sample */
    uint32_t ratio_q8;
    int32_t mod_depth, feedback;
    int32_t noise_a;         /* one-pole coefficient, Q15 */
    uint8_t noise_highpass;
    int32_t noise_level;     /* Q15 */
    int32_t level;           /* Q15 */
    int32_t pan_l, pan_r;    /* Q15 */
    uint32_t sample_step;    /* Q16 frames per output frame */
};

struct drum_voice_state {
    uint8_t active;
    uint16_t fade;           /* choke fade-out, frames left */
    uint32_t car_phase, mod_phase;
    int32_t mod_prev;
    uint32_t amp_env, pitch_env, mod_env, noise_env; /* Q30 */
    uint32_t pos_q16;
    uint32_t noise;
    int32_t lp;
    int32_t gain;            /* Q15: level x velocity */
};

struct drum_sample {
    const int16_t *pcm;
    uint32_t frames;
};

struct drums {
    struct drum_derived derived[DRUM_VOICES];
    struct drum_voice_state voice[DRUM_VOICES];
    struct drum_sample sample;   /* slot 0: the uploaded kick */
};

/* Default kit: kick (sample if loaded, else FM), snare, hats, toms ... */
void drum_kit_default(struct drum_kit *kit);
/* Turn a voice into the FM kick (used for voice 1 when no kick is uploaded). */
void drum_voice_fm_kick(struct drum_voice_params *voice);
const char *drum_engine_name(unsigned engine);
const char *drum_param_name(unsigned param);

void drums_init(struct drums *d);
void drums_prepare(struct drums *d, const struct drum_kit *kit);   /* control task */
void drums_prepare_voice(struct drums *d, const struct drum_kit *kit, unsigned v);

/* Render frames into out (interleaved stereo Q23, added to what is there),
 * triggering events at their frame offsets. Audio interrupt. */
void drums_render(struct drums *d, int32_t *out, unsigned frames,
                  const struct seq_event *events, unsigned n_events);

/* Start a voice now. Audio interrupt only (live pads go through a queue the
 * audio code drains, see app/main.c). */
void drums_trigger(struct drums *d, unsigned voice, unsigned velocity);

#endif
