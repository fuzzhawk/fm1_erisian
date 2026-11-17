/* SPDX-License-Identifier: MIT */
/* ERISIAN drum voices: 11 voices, each with one engine (Machinedrum style),
 * followed by a common chain: transient -> drive -> filter -> lo-fi ->
 * texture -> level/pan.
 *
 * Engines: SAMPLE (uploaded kick), FM (2-op + noise), KS (Karplus-Strong),
 * METAL (inharmonic squares, reverse swell), SHAKER (particle model),
 * OBJECT (modal klunk-bonk-crunch + rattle), NOISE FX (riser/fall/impact).
 *
 * drums_prepare() turns 0..127 parameters into fixed-point constants
 * (control task: divisions and loops allowed). drums_render() runs in the
 * audio interrupt: integer maths, RAM only, no libc.
 */
#ifndef ERISIAN_DRUMS_H
#define ERISIAN_DRUMS_H

#include <stdint.h>

#include "seq.h"

#define DRUM_VOICES SEQ_VOICES
#define DRUM_CHOKE_GROUPS 4u
#define DRUM_EP 13u               /* engine parameters; meaning per engine */
#define DRUM_KS_LEN 1024u         /* Karplus-Strong delay line (>= 43 Hz) */

enum drum_engine {
    DRUM_OFF, DRUM_SAMPLE, DRUM_FM, DRUM_KS, DRUM_METAL, DRUM_SHAKER,
    DRUM_OBJECT, DRUM_NOISE, DRUM_ENGINES
};

/* By convention e[0] is TUNE and e[1] is DECAY for every engine. */
enum { E_TUNE, E_DECAY };
/* FM */
enum { FM_TUNE, FM_DECAY, FM_PENV, FM_PDEC, FM_RATIO, FM_MOD, FM_MDEC,
       FM_FDBK, FM_NOISE, FM_NDEC, FM_NTONE };
/* KS */
enum { KS_TUNE, KS_DECAY, KS_EXCITE, KS_BRIGHT, KS_STRETCH, KS_BEND, KS_NOISE, KS_NDEC };
/* METAL */
enum { MT_TUNE, MT_DECAY, MT_TONE, MT_COLOR, MT_RING, MT_REVERSE };
/* SHAKER */
enum { SH_TUNE, SH_DECAY, SH_RESO, SH_BEADS, SH_BDEC, SH_TUNE2, SH_MIX };
/* OBJECT */
enum { OB_TUNE, OB_DECAY, OB_MATERIAL, OB_HARD, OB_CRUNCH, OB_CDEC, OB_RATTLE };
/* NOISE FX */
enum { NZ_TUNE, NZ_DECAY, NZ_MODE, NZ_LENGTH, NZ_RANGE, NZ_RESO, NZ_SUB };

/* Common chain, the same for every engine. */
enum drum_common {
    DC_LEVEL, DC_PAN, DC_FTYPE, DC_CUTOFF, DC_RESO, DC_DRIVE, DC_FENV,
    DC_BITS, DC_RATE, DC_TRANS, DC_TLEVEL, DC_TEXTURE, DC_XLEVEL, DC_FDECAY,
    DC_COUNT
};

/* What a kit stores per voice (saved with patterns and kits). */
struct drum_voice_params {
    uint8_t engine;          /* enum drum_engine */
    uint8_t choke;           /* 0 = none, 1..4 */
    uint8_t e[DRUM_EP];      /* engine parameters, 0..127 */
    uint8_t c[DC_COUNT];     /* common chain, 0..127 */
};

struct drum_kit {
    struct drum_voice_params voice[DRUM_VOICES];
};

/* Kit layout saved by ERISIAN builds before the engine refactor. */
struct drum_voice_params_v1 { uint8_t engine, choke, p[13]; };
struct drum_kit_v1 { struct drum_voice_params_v1 voice[DRUM_VOICES]; };
void drum_kit_from_v1(struct drum_kit *kit, const struct drum_kit_v1 *old);

/* Names for the UI: engine, its parameter labels (NULL = unused), common. */
const char *drum_engine_name(unsigned engine);
const char *drum_engine_param_name(unsigned engine, unsigned index);
const char *drum_common_name(unsigned index);

/* Fixed-point form of one voice's parameters (control task writes). */
struct drum_derived {
    uint8_t engine, choke;
    /* engine */
    uint32_t inc;            /* main phase increment */
    uint32_t sweep;          /* FM: extra increment at full pitch env */
    uint32_t amp_coef, coef2, coef3, coef4;   /* Q30 per-sample decays */
    uint32_t ratio_q8;
    int32_t a, b, k;         /* engine-specific Q15/Q14 constants */
    int32_t mode;
    uint32_t period_q16;     /* KS: delay in samples, Q16 */
    int32_t loop_gain;       /* KS: Q15 gain per pass */
    uint32_t length;         /* METAL reverse / NOISE: frames */
    uint32_t ramp_inc;       /* Q30 per frame */
    int32_t res_b1[4], res_b2[4]; /* OBJECT: resonators, Q30 */
    int32_t res_gain[4];     /* Q15 */
    uint32_t sample_step;    /* SAMPLE: Q16 */
    uint32_t incs[6];        /* METAL partials / OBJECT mallet */
    uint32_t trans_coef;     /* transient decay, Q30 */
    int32_t noise_a;         /* one-pole noise filter, Q15 */
    uint8_t noise_highpass;
    int32_t noise_level;     /* Q15 */
    /* common chain */
    uint8_t ftype;           /* 0 off, 1 LP, 2 HP, 3 BP */
    uint8_t cutoff;          /* 0..127 index */
    int8_t fenv;             /* -64..63 index offset at full envelope */
    uint32_t fenv_coef;      /* Q30 */
    int32_t fq;              /* damping, Q14 */
    int32_t drive;           /* Q8 gain (256 = 1x) */
    uint8_t crush_shift;     /* bits dropped from Q15 */
    uint8_t hold;            /* sample-and-hold period, frames */
    uint8_t trans;           /* 0 none, 1 click, 2 snap, 3 thump */
    int32_t trans_level;     /* Q15 */
    uint8_t texture;         /* 0 none, 1 crackle, 2 hiss, 3 both */
    int32_t tex_level;       /* Q15 */
    int32_t level;           /* Q15 */
    int32_t pan_l, pan_r;    /* Q15 */
};

struct drum_voice_state {
    uint8_t active;
    uint16_t fade;           /* choke fade-out, frames left */
    uint32_t phase[6];
    int32_t s0, s1, s2, s3;  /* engine filter/feedback state */
    uint32_t env, env2, env3, env4; /* Q30 */
    uint32_t pos;            /* SAMPLE Q16 / KS write index / ramps */
    uint32_t count;          /* frames since trigger */
    uint32_t noise;
    int32_t ry1[4], ry2[4];  /* OBJECT resonators */
    /* common chain */
    uint32_t fenv;           /* Q30 */
    int32_t low, band;       /* SVF */
    int32_t held; uint8_t hold_count;
    uint32_t tenv;           /* transient envelope Q30 */
    uint32_t tphase;
    uint32_t xenv;           /* texture envelope Q30 */
    int32_t xlp;
    int32_t gain;            /* Q15: level x velocity */
};

struct drum_sample {
    const int16_t *pcm;
    uint32_t frames;
};

struct drums {
    struct drum_derived derived[DRUM_VOICES];
    struct drum_voice_state voice[DRUM_VOICES];
    int16_t ks[DRUM_VOICES][DRUM_KS_LEN];
    struct drum_sample sample;   /* slot 0: the uploaded kick */
    uint32_t step_frames_q8;     /* one 1/16 step at the current tempo */
};

void drum_kit_default(struct drum_kit *kit);
/* Turn a voice into the FM kick (used for voice 1 when no kick is uploaded). */
void drum_voice_fm_kick(struct drum_voice_params *voice);
/* Defaults for a voice switched to another engine. */
void drum_voice_engine_defaults(struct drum_voice_params *voice, unsigned engine);

void drums_init(struct drums *d);
void drums_set_tempo(struct drums *d, const struct drum_kit *kit, unsigned bpm100);
void drums_prepare(struct drums *d, const struct drum_kit *kit);   /* control task */
void drums_prepare_voice(struct drums *d, const struct drum_kit *kit, unsigned v);

/* Render frames into out (interleaved stereo Q23, added to what is there),
 * triggering events at their frame offsets. Audio interrupt. */
void drums_render(struct drums *d, int32_t *out, unsigned frames,
                  const struct seq_event *events, unsigned n_events);

/* Start a voice now. Audio interrupt only (live pads go through a queue the
 * audio code drains, see app/audio.c). */
void drums_trigger(struct drums *d, unsigned voice, unsigned velocity);

#endif
