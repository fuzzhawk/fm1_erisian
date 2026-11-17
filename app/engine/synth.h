/* SPDX-License-Identifier: MIT */
/* ERISIAN keys: 3 layers sharing a pool of voices.
 *
 * Each layer has an engine, a key zone (layers whose zones overlap stack),
 * an octave, a play mode (poly, mono, legato) and a common chain:
 * engine -> drive -> filter (with envelope) -> amp ADSR -> level/pan.
 *
 * Engines ("cheap first" models): 808 BASS (pitch envelope, drive -> fold ->
 * tone, click, sub), FM LEAD (2-op, mod envelope, feedback, vibrato), KOTO
 * (Karplus-Strong string, pluck position, damping, body), FLUTE (tone plus
 * a breath resonator, chiff, vibrato), VIBES (three modal bars, mallet,
 * motor tremolo), PAD (noise through a four-resonator bank).
 *
 * synth_prepare() turns 0..127 parameters into fixed-point constants
 * (control task). synth_note() and synth_render() run in the audio
 * interrupt: integer maths, RAM only, no libc, no division.
 */
#ifndef ERISIAN_SYNTH_H
#define ERISIAN_SYNTH_H

#include <stdint.h>

#define SYNTH_LAYERS 3u
#define SYNTH_VOICES 6u           /* shared pool (tune on hardware) */
#define SYNTH_EP 8u               /* engine parameters; meaning per engine */
#define SYNTH_KS_LEN 1024u        /* KOTO delay line (>= 43 Hz) */
#define SYNTH_HELD 8u             /* mono note stack per layer */

enum synth_engine {
    SY_OFF, SY_BASS, SY_LEAD, SY_KOTO, SY_FLUTE, SY_VIBES, SY_PAD, SY_ENGINES
};
enum synth_mode { SM_POLY, SM_MONO, SM_LEGATO, SM_MODES };

/* e[0] is TUNE (fine, +-1 semitone) for every engine. */
enum { SE_TUNE };
enum { BA_TUNE, BA_PENV, BA_PDEC, BA_DRIVE, BA_FOLD, BA_TONE, BA_CLICK, BA_SUB };
enum { LE_TUNE, LE_RATIO, LE_MOD, LE_MDEC, LE_MSUS, LE_FDBK, LE_VRATE, LE_VDEPTH };
enum { KO_TUNE, KO_PLUCK, KO_DAMP, KO_DECAY, KO_BRIGHT, KO_BODY };
enum { FL_TUNE, FL_BREATH, FL_TONE, FL_CHIFF, FL_VRATE, FL_VDEPTH, FL_AIR };
enum { VI_TUNE, VI_HARD, VI_DECAY, VI_TONE2, VI_TONE3, VI_TRATE, VI_TDEPTH };
enum { PA_TUNE, PA_BRIGHT, PA_RESO, PA_SPREAD, PA_AIR };

enum synth_common {
    SC_LEVEL, SC_PAN, SC_ATTACK, SC_DECAY, SC_SUSTAIN, SC_RELEASE,
    SC_FTYPE, SC_CUTOFF, SC_RESO, SC_FENV, SC_FDECAY, SC_DRIVE, SC_GLIDE,
    SC_COUNT
};

struct synth_layer_params {
    uint8_t engine;          /* enum synth_engine */
    uint8_t mode;            /* enum synth_mode */
    uint8_t lo, hi;          /* key zone, MIDI notes (after the keyboard octave) */
    int8_t octave;           /* -3..+3 */
    uint8_t e[SYNTH_EP];     /* engine parameters, 0..127 */
    uint8_t c[SC_COUNT];     /* common chain, 0..127 */
};

struct synth_patch {
    struct synth_layer_params layer[SYNTH_LAYERS];
};

/* Fixed-point form of a layer (control task writes). */
struct synth_derived {
    uint8_t engine, mode, lo, hi;
    int8_t transpose;        /* semitones */
    uint32_t fine_q16;       /* fine tune multiplier */
    /* amp ADSR */
    uint32_t attack_inc, decay_coef, sustain, release_coef;   /* Q30 */
    /* filter */
    uint8_t ftype, cutoff;
    int8_t fenv;
    uint32_t fenv_coef;
    int32_t fq;              /* Q14 damping */
    int32_t drive;           /* Q8, 256 = off */
    uint32_t glide_k;        /* Q16 per-sample slew, 65536 = instant */
    int32_t level, pan_l, pan_r;   /* Q15 */
    /* engine */
    uint32_t coef1, coef2;   /* Q30 per-sample decays */
    uint32_t lfo_inc;        /* vibrato / tremolo */
    int32_t a, b, c, d, k;   /* engine constants, Q15 unless noted */
    uint32_t ratio_q8[4];    /* partial / modulator ratios, Q8 */
    int32_t gain[4];         /* partial gains, Q15 */
    uint32_t r_q30[4];       /* resonator radius */
};

struct synth_voice {
    uint8_t active, layer, key, gate, stage;
    uint32_t age;
    uint32_t inc, target;    /* pitch: current and glide target */
    uint32_t phase[4], lfo;
    uint32_t env;            /* amp, Q30 */
    uint32_t fenv, env1, env2;   /* filter and engine envelopes, Q30 */
    int32_t s0, s1, s2;      /* engine state */
    int32_t ry1[4], ry2[4], rb1[4], rb2[4], rg[4];   /* resonators (Q30 coefs) */
    int32_t low, band;       /* SVF */
    uint32_t noise;
    uint32_t pos, period_q16;   /* KOTO */
    int32_t vel;             /* Q15 */
};

struct synth {
    struct synth_derived derived[SYNTH_LAYERS];
    struct synth_voice voice[SYNTH_VOICES];
    int16_t ks[SYNTH_VOICES][SYNTH_KS_LEN];
    uint8_t held[SYNTH_LAYERS][SYNTH_HELD];   /* mono note stacks (keys) */
    uint8_t held_n[SYNTH_LAYERS];
    uint32_t clock;          /* voice ages */
};

void synth_patch_default(struct synth_patch *patch);
/* Defaults for a layer switched to another engine (engine params, ADSR, mode). */
void synth_layer_engine_defaults(struct synth_layer_params *layer, unsigned engine);

const char *synth_engine_name(unsigned engine);
const char *synth_engine_param_name(unsigned engine, unsigned index);   /* NULL = unused */
const char *synth_common_name(unsigned index);
const char *synth_mode_name(unsigned mode);

void synth_init(struct synth *s);
void synth_prepare(struct synth *s, const struct synth_patch *patch);   /* control task */
void synth_prepare_layer(struct synth *s, const struct synth_patch *patch, unsigned layer);

/* Audio interrupt. key = MIDI note from the keyboard; velocity 0 = note off. */
void synth_note(struct synth *s, unsigned key, unsigned velocity);
void synth_all_off(struct synth *s);
/* Add frames of interleaved stereo Q23 to out. */
void synth_render(struct synth *s, int32_t *out, unsigned frames);

#endif
