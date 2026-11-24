/* SPDX-License-Identifier: MIT */
/* MAVIS engine core: voice pool, pitch and tunings, and the KOTO waveguide
 * string (more engines join in later phases, see docs/MAVIS.md).
 *
 * Control task: mv_prepare() turns 0..127 parameters into fixed-point
 * constants and queues notes. Audio interrupt: mv_note(), mv_render(): integer
 * maths, RAM only, no libc, no division.
 */
#ifndef MAVIS_ENGINE_H
#define MAVIS_ENGINE_H

#include <stdint.h>

#define MV_MAX_VOICES 8u
#define MV_KS_LEN 2048u                /* string delay line: >= 21.6 Hz */
#define MV_PARAMS 32u
#define MV_MODES 12u                   /* MALLET: modes per note */
#define MV_RES (2u * MV_MODES)         /* ... doubled by unison strings */

enum mv_engine { MV_KOTO, MV_MALLET, MV_ENGINES };

/* KOTO parameters (0..127). */
enum {
    KT_DECAY, KT_BRIGHT, KT_STIFF, KT_DAMP,          /* STRING */
    KT_POS, KT_NAIL, KT_STRENGTH, KT_NOISE,          /* PLUCK */
    KT_BODY, KT_SIZE, KT_SAWARI, KT_TENSION,         /* BODY */
    KT_OSHIDE, KT_PRESS, KT_BENDIN, KT_BENDTIME,     /* PRESS: oshide, bend-in */
    KT_ATO, KT_ATODELAY, KT_YURI, KT_YURIRATE,       /* MOTION: ato-oshi, yuri */
    KT_HUMAN, KT_WIDTH, KT_LEVEL,                    /* OUTPUT */
    KT_COUNT
};

/* MALLET parameters (0..127). MATERIAL runs kalimba -> glockenspiel ->
 * xylophone -> vibraphone -> marimba -> piano string. */
enum {
    MA_MATERIAL, MA_TONE, MA_DECAY, MA_DAMP,         /* MATERIAL */
    MA_HARD, MA_POS, MA_FORCE, MA_NOISE,             /* MALLET */
    MA_TUBE, MA_BODY, MA_TREM, MA_TRATE,             /* RESONATE */
    MA_UNISON, MA_SUSTAIN, MA_STRETCH, MA_RELEASE,   /* STRINGS (piano end) */
    MA_HUMAN, MA_WIDTH, MA_LEVEL,                    /* OUTPUT */
    MA_COUNT
};

/* Tuning and keyboard (global, not per patch). */
enum mv_tuning { TU_EQUAL, TU_JUST, TU_PYTH, TU_COUNT };
enum mv_scale { SC_CHROMATIC, SC_MAJOR, SC_MINOR, SC_HIRAJOSHI, SC_KUMOI, SC_KOKIN,
                SC_IWATO, SC_AKEBONO, SC_YO, SC_COUNT };
enum mv_map { MAP_KEYS, MAP_WHITE, MAP_COUNT };

struct mv_patch {
    uint8_t engine;
    uint8_t p[MV_PARAMS];
};

struct mv_global {
    uint8_t tuning, scale, map, root;   /* root: 0 = C */
    uint8_t voices;                     /* 1..MV_MAX_VOICES */
};

/* Fixed-point form of a MALLET patch (control task writes). */
struct mallet_derived {
    uint8_t modes;               /* modes in use (<= MV_MODES) */
    uint32_t ratio_q16[MV_MODES];
    int32_t amp[MV_MODES];       /* Q15: material x tone x strike position */
    uint32_t atten_q32[MV_MODES];/* log2 attenuation per sample at the mode */
    uint32_t damp_k;             /* frequency damping: x (1 + inc * k / 2^32) */
    uint32_t unison_q16;         /* detune of the second string, 0 = none */
    uint32_t sustain_q16;        /* second string decays this much slower (<= 1) */
    uint32_t mallet_inc;         /* half-sine contact pulse */
    int32_t mallet_gain;         /* excitation scale for the pulse length, Q15 */
    int32_t force;               /* Q15 */
    int32_t noise;               /* impact click, Q15 */
    int32_t tube;                /* tube resonator mix, Q15 */
    int32_t trem_depth;          /* Q15 */
    uint32_t trem_inc;           /* per sample */
    uint32_t release_q30;        /* damper: amplitude factor per sample, Q30; 0 = ring */
    int32_t human_cents_q16, human_gain, width, level;
    int32_t body_b1[3], body_b2[3], body_g[3], body_mix;   /* soundboard */
};

/* Fixed-point form of a KOTO patch (control task writes). */
struct koto_derived {
    uint32_t atten_q32;      /* log2 attenuation per sample, Q32 */
    uint32_t release_q32;    /* extra attenuation after key up */
    int32_t s;               /* loop filter: y = (1-s) x + s x1, Q15 */
    int32_t ap;              /* dispersion allpass coefficient, Q15 (<= 0) */
    uint32_t comp_q16;       /* loop delay to subtract (filter + allpass) */
    int32_t pos;             /* pluck / pickup position, Q15 of the period */
    uint32_t exc_len, exc_inc;   /* excitation burst: frames, half-sine step */
    int32_t exc_amp;         /* Q15 */
    int32_t exc_lp;          /* excitation low-pass, Q15 */
    int32_t exc_noise;       /* noise vs smooth pulse, Q15 */
    int32_t saw_th, saw_r;   /* sawari: contact threshold, slope Q15 */
    int32_t tension_q16;     /* attack pitch offset, semitones Q16 */
    int32_t oshide_q16, press_k;     /* press amount; glide per block, Q16 */
    int32_t bendin_q16, bendin_k;
    int32_t ato_q16;
    uint32_t ato_delay;      /* blocks */
    int32_t yuri_q16;        /* vibrato depth, semitones Q16 */
    uint32_t yuri_inc;       /* per block */
    int32_t human_cents_q16, human_gain;  /* humanize range */
    int32_t width;           /* note spread across the stereo field, Q15 */
    int32_t level;           /* Q15 */
    /* shared body: three resonators on the koto sum */
    int32_t body_b1[3], body_b2[3], body_g[3];   /* Q30 (state runs at Q23) */
    int32_t body_mix;        /* Q15 */
};

struct mv_voice {
    uint8_t active, gate, note, pending;
    uint8_t engine;          /* the engine that started it */
    uint8_t pending_note;
    uint32_t age;            /* allocation order */
    uint32_t blocks;         /* since note on */
    int32_t pitch_q16;       /* note + tuning + humanize, semitones Q16 */
    int32_t bend_q16, press_q16, bendin_q16, ato_q16;
    uint32_t lfo;
    int32_t gain;            /* Q15: level x humanize */
    int32_t pan_l, pan_r;    /* Q15 */
    int32_t quiet;           /* blocks below the silence threshold */
    /* string */
    uint32_t w;              /* write index */
    uint32_t pint;           /* integer read delay */
    int32_t c0, c1, c2, c3;  /* Lagrange taps, Q15 */
    int32_t loop_gain;       /* Q16 */
    uint32_t comb;           /* pluck-position delay, samples */
    int32_t x1, ap_x1, ap_y1;
    uint32_t exc_i, exc_phase;
    int32_t exc_lpf;
    uint32_t noise;
    int32_t tension_env;     /* Q16 */
    int32_t peak;
    /* mallet */
    uint8_t nres;            /* resonators in use */
    int32_t rb1[MV_RES], rb2[MV_RES], rg[MV_RES], ry1[MV_RES], ry2[MV_RES];
    int32_t tb1, tb2, tg, ty1, ty2;      /* tube */
    uint32_t mallet_phase;
    uint8_t mallet_on;
    uint16_t click;          /* impact noise frames left */
    uint32_t rel_env;        /* damper envelope, Q30 */
};

struct mavis_engine {
    struct mv_global global;
    struct koto_derived koto;
    struct mallet_derived mallet;
    uint8_t engine;          /* the patch's engine */
    uint32_t trem_phase;     /* vibraphone motor: one for all bars */
    /* Adaptive detail: resonators to share between sounding voices, steered
     * by the measured CPU load (audio.c sets load_q16 every block). */
    uint32_t res_budget;
    uint32_t load_q16;
    int32_t tuning_q16[12];  /* per pitch class from the root, semitones Q16 */
    struct mv_voice voice[MV_MAX_VOICES];
    int16_t line[MV_MAX_VOICES][MV_KS_LEN];
    int32_t body_y1[3], body_y2[3];
    int32_t board_y1[3], board_y2[3];
    uint8_t pressing;        /* oshide held (all sounding notes) */
    uint32_t clock;
    uint32_t random;
    uint8_t last_voice;      /* for the visualiser: newest voice */
};

void mv_patch_default(struct mv_patch *patch, unsigned engine);
void mv_global_default(struct mv_global *global);
void mv_init(struct mavis_engine *e);
void mv_prepare(struct mavis_engine *e, const struct mv_patch *patch, const struct mv_global *g);

const char *mv_engine_name(unsigned engine);
const char *mv_param_name(unsigned engine, unsigned index);   /* NULL = unused */
const char *mv_tuning_name(unsigned t);
const char *mv_scale_name(unsigned s);
const char *mv_map_name(unsigned m);

/* Keyboard key (0..26 from F3) and octave to a MIDI note, by the map and
 * scale: MAP_KEYS plays every key chromatically; MAP_WHITE plays the scale on
 * the white keys, and a black key plays the white key below it a semitone up
 * (like pressing the string). -1 = no note. Control task. */
int mv_key_note(const struct mv_global *g, unsigned key, int octave);

/* Audio interrupt. velocity 0 = note off. */
void mv_note(struct mavis_engine *e, unsigned note, unsigned velocity);
void mv_all_off(struct mavis_engine *e);
void mv_render(struct mavis_engine *e, int32_t *out, unsigned frames);

#endif
