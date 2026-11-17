/* SPDX-License-Identifier: MIT */
#include "drums.h"

#include <string.h>

#include "fm1.h"
#include "tables.h"

#define Q30 (1u << 30)
#define CHOKE_FADE 64u
#define RATE 44100u

/* --- names ----------------------------------------------------------------- */

static const char *const engine_names[DRUM_ENGINES] = {
    "OFF", "SAMPLE", "FM", "KS", "METAL", "SHAKER", "OBJECT", "NOISE",
};
static const char *const engine_params[DRUM_ENGINES][DRUM_EP] = {
    [DRUM_SAMPLE] = { "TUNE", "DECAY" },
    [DRUM_FM] = { "TUNE", "DECAY", "P.ENV", "P.DEC", "RATIO", "MOD", "M.DEC", "FDBK",
                  "NOISE", "N.DEC", "N.TON" },
    [DRUM_KS] = { "TUNE", "DECAY", "EXCIT", "BRITE", "STRCH", "BEND", "NOISE", "N.DEC" },
    [DRUM_METAL] = { "TUNE", "DECAY", "TONE", "COLOR", "RING", "REVRS" },
    [DRUM_SHAKER] = { "TUNE", "DECAY", "RESO", "BEADS", "B.DEC", "TUNE2", "MIX" },
    [DRUM_OBJECT] = { "TUNE", "DECAY", "MATER", "HARD", "CRNCH", "C.DEC", "RATTL" },
    [DRUM_NOISE] = { "TUNE", "DECAY", "MODE", "LEN", "RANGE", "RESO", "SUB" },
};
static const char *const common_names[DC_COUNT] = {
    "LEVEL", "PAN", "F.TYP", "CUTOF", "RESO", "DRIVE", "F.ENV",
    "BITS", "RATE", "TRANS", "T.LVL", "TEXTR", "X.LVL", "F.DEC",
};

const char *drum_engine_name(unsigned e) { return e < DRUM_ENGINES ? engine_names[e] : "?"; }
const char *drum_engine_param_name(unsigned e, unsigned i)
{
    return e < DRUM_ENGINES && i < DRUM_EP ? engine_params[e][i] : 0;
}
const char *drum_common_name(unsigned i) { return i < DC_COUNT ? common_names[i] : "?"; }

/* --- kit ------------------------------------------------------------------- */

static void common_defaults(struct drum_voice_params *v)
{
    static const uint8_t c[DC_COUNT] = {
        [DC_LEVEL] = 100, [DC_PAN] = 64, [DC_FTYPE] = 0, [DC_CUTOFF] = 127,
        [DC_RESO] = 0, [DC_DRIVE] = 0, [DC_FENV] = 64, [DC_BITS] = 127,
        [DC_RATE] = 0, [DC_TRANS] = 0, [DC_TLEVEL] = 64, [DC_TEXTURE] = 0,
        [DC_XLEVEL] = 40, [DC_FDECAY] = 40,
    };
    memcpy(v->c, c, sizeof c);
}

static void set_e(struct drum_voice_params *v, unsigned engine, const uint8_t *e, unsigned n)
{
    v->engine = (uint8_t)engine;
    memset(v->e, 0, sizeof v->e);
    memcpy(v->e, e, n);
}

void drum_voice_engine_defaults(struct drum_voice_params *v, unsigned engine)
{
    static const uint8_t sample[] = { 64, 127 };
    static const uint8_t fm[] = { 62, 45, 30, 15, 3, 30, 20, 0, 90, 50, 95 };
    static const uint8_t ks[] = { 40, 70, 0, 70, 0, 20, 20, 20 };
    static const uint8_t metal[] = { 70, 30, 110, 0, 40, 0 };
    static const uint8_t shaker[] = { 100, 40, 60, 70, 20, 115, 40 };
    static const uint8_t object[] = { 60, 60, 0, 90, 0, 30, 0 };
    static const uint8_t noise[] = { 40, 80, 0, 16, 60, 70, 90 };
    switch (engine) {
    case DRUM_SAMPLE: set_e(v, engine, sample, sizeof sample); break;
    case DRUM_FM: set_e(v, engine, fm, sizeof fm); break;
    case DRUM_KS: set_e(v, engine, ks, sizeof ks); break;
    case DRUM_METAL: set_e(v, engine, metal, sizeof metal); break;
    case DRUM_SHAKER: set_e(v, engine, shaker, sizeof shaker); break;
    case DRUM_OBJECT: set_e(v, engine, object, sizeof object); break;
    case DRUM_NOISE: set_e(v, engine, noise, sizeof noise); break;
    default: v->engine = DRUM_OFF; break;
    }
}

void drum_voice_fm_kick(struct drum_voice_params *v)
{
    static const uint8_t kick[] = { 30, 70, 70, 25, 1, 10, 20, 0, 0, 10, 40 };
    set_e(v, DRUM_FM, kick, sizeof kick);
}

void drum_kit_default(struct drum_kit *k)
{
    memset(k, 0, sizeof *k);
    for (unsigned v = 0; v < DRUM_VOICES; v++) common_defaults(&k->voice[v]);
    static const uint8_t snare[] = { 62, 45, 30, 15, 3, 30, 20, 0, 90, 50, 95 };
    static const uint8_t chh[] = { 75, 22, 112, 0, 30, 0 };
    static const uint8_t ohh[] = { 75, 62, 112, 0, 30, 0 };
    static const uint8_t ltom[] = { 34, 72, 0, 60, 0, 25, 25, 15 };
    static const uint8_t htom[] = { 46, 68, 0, 70, 0, 25, 25, 15 };
    static const uint8_t clap[] = { 70, 30, 0, 0, 1, 0, 0, 0, 110, 35, 85 };
    static const uint8_t wood[] = { 70, 45, 0, 100, 0, 30, 0 };
    static const uint8_t shake[] = { 100, 38, 60, 70, 18, 115, 40 };
    static const uint8_t fkick[] = { 24, 80, 90, 35, 0, 25, 30, 0, 0, 10, 40 };
    static const uint8_t revcy[] = { 60, 70, 100, 64, 50, 32 };
    drum_voice_engine_defaults(&k->voice[0], DRUM_SAMPLE);
    set_e(&k->voice[1], DRUM_FM, snare, sizeof snare);
    set_e(&k->voice[2], DRUM_METAL, chh, sizeof chh);
    set_e(&k->voice[3], DRUM_METAL, ohh, sizeof ohh);
    set_e(&k->voice[4], DRUM_KS, ltom, sizeof ltom);
    set_e(&k->voice[5], DRUM_KS, htom, sizeof htom);
    set_e(&k->voice[6], DRUM_FM, clap, sizeof clap);
    set_e(&k->voice[7], DRUM_OBJECT, wood, sizeof wood);
    set_e(&k->voice[8], DRUM_SHAKER, shake, sizeof shake);
    set_e(&k->voice[9], DRUM_FM, fkick, sizeof fkick);
    set_e(&k->voice[10], DRUM_METAL, revcy, sizeof revcy);
    k->voice[2].choke = 1;   /* closed and open hat choke each other */
    k->voice[3].choke = 1;
    k->voice[8].c[DC_FTYPE] = 64;   /* shaker through a high-pass */
    k->voice[8].c[DC_CUTOFF] = 80;
}

void drum_kit_from_v1(struct drum_kit *kit, const struct drum_kit_v1 *old)
{
    memset(kit, 0, sizeof *kit);
    for (unsigned v = 0; v < DRUM_VOICES; v++) {
        const struct drum_voice_params_v1 *o = &old->voice[v];
        struct drum_voice_params *n = &kit->voice[v];
        common_defaults(n);
        n->engine = o->engine < 3 ? o->engine : DRUM_OFF;   /* OFF, SAMPLE, FM */
        n->choke = o->choke;
        n->c[DC_LEVEL] = o->p[0];
        n->c[DC_PAN] = o->p[1];
        for (unsigned i = 0; i < 11; i++)      /* TUNE .. N.TONE */
            n->e[i] = o->p[2 + i];
    }
}

/* --- prepare (control task) ----------------------------------------------- */

static unsigned clamp127(int v) { return v < 0 ? 0u : v > 127 ? 127u : (unsigned)v; }

static uint32_t pow_q30(uint32_t c, uint32_t n)
{
    uint64_t r = Q30, b = c;
    while (n) {
        if (n & 1) r = (r * b) >> 30;
        b = (b * b) >> 30;
        n >>= 1;
    }
    return (uint32_t)r;
}

static uint32_t step_frames(const struct drums *d) { return d->step_frames_q8 >> 8; }

static const uint16_t metal_ratios[4][6] = {      /* Q8, inharmonic sets */
    { 256, 363, 479, 648, 694, 937 },
    { 256, 341, 436, 595, 731, 1003 },
    { 256, 402, 523, 597, 811, 1166 },
    { 256, 289, 377, 512, 563, 868 },
};
static const uint16_t object_ratios[5][4] = {     /* Q8: mode frequency ratios */
    { 256, 658, 1078, 1766 },   /* wood */
    { 256, 468, 755, 1055 },    /* plastic */
    { 256, 707, 1382, 2286 },   /* tin */
    { 256, 384, 896, 1408 },    /* bottle */
    { 256, 594, 1088, 1697 },   /* glass */
};
static const int8_t object_damp[5][4] = {         /* decay index offsets per mode */
    { 0, -18, -30, -40 }, { 0, -10, -18, -26 }, { 0, -2, -4, -6 },
    { 0, -6, -14, -22 }, { 0, -3, -6, -9 },
};

void drums_prepare_voice(struct drums *d, const struct drum_kit *kit, unsigned v)
{
    const struct drum_voice_params *p = &kit->voice[v];
    struct drum_derived x;
    memset(&x, 0, sizeof x);
    x.engine = p->engine < DRUM_ENGINES ? p->engine : DRUM_OFF;
    x.choke = p->choke <= DRUM_CHOKE_GROUPS ? p->choke : 0;
    const uint8_t *e = p->e;
    x.inc = table_tune[clamp127(e[E_TUNE])];
    x.amp_coef = table_decay[clamp127(e[E_DECAY])];

    switch (x.engine) {
    case DRUM_SAMPLE: {
        int st = (int)clamp127(e[E_TUNE]) - 64;
        int idx = 64 + st * 191 / 640;   /* +-1 octave on the 6.6-octave tune table */
        idx = idx < 0 ? 0 : idx > 127 ? 127 : idx;
        x.sample_step = (uint32_t)(((uint64_t)table_tune[idx] << 16) / table_tune[64]);
        break;
    }
    case DRUM_FM: {
        uint64_t sweep = (uint64_t)x.inc * e[FM_PENV] * 15u / 127u;
        x.sweep = sweep > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)sweep;
        x.coef2 = table_decay[clamp127(e[FM_PDEC])];
        x.coef3 = table_decay[clamp127(e[FM_MDEC])];
        x.coef4 = table_decay[clamp127(e[FM_NDEC])];
        x.ratio_q8 = table_ratio[e[FM_RATIO] & 15u];
        x.a = (int32_t)e[FM_MOD] * 1314;    /* modulation index up to ~8 rad */
        x.b = (int32_t)e[FM_FDBK] * 160;
        unsigned tone = clamp127(e[FM_NTONE]);
        x.noise_highpass = tone >= 64;
        unsigned t = x.noise_highpass ? tone - 64 : tone;
        x.noise_a = (int32_t)(t + 1) * 512;
        if (x.noise_highpass) x.noise_a = 32768 - x.noise_a + 512;
        x.noise_level = (int32_t)e[FM_NOISE] * 258;
        break;
    }
    case DRUM_KS: {
        uint64_t period = ((uint64_t)1 << 48) / x.inc;          /* samples, Q16 */
        if (period > (uint64_t)(DRUM_KS_LEN - 8) << 16) period = (uint64_t)(DRUM_KS_LEN - 8) << 16;
        if (period < (4u << 16)) period = 4u << 16;
        x.period_q16 = (uint32_t)period - 32768u;               /* loop filter delay */
        x.loop_gain = (int32_t)(pow_q30(x.amp_coef, (uint32_t)(period >> 16)) >> 15);
        x.mode = e[KS_EXCITE] >> 5;                            /* noise, click, sample, mallet */
        x.noise_a = (int32_t)(clamp127(e[KS_BRIGHT]) + 1) * 256;  /* excitation low-pass */
        x.k = 16384 - (int32_t)clamp127(e[KS_BRIGHT]) * 100;   /* loop averaging */
        x.a = (int32_t)clamp127(e[KS_STRETCH]) * 200;          /* dispersion allpass */
        x.sweep = (uint32_t)(((uint64_t)x.period_q16 * clamp127(e[KS_BEND])) >> 8);
        x.coef2 = table_decay[clamp127(e[KS_BEND] ? 40 : 0)];
        x.noise_level = (int32_t)e[KS_NOISE] * 258;
        x.coef4 = table_decay[clamp127(e[KS_NDEC])];
        break;
    }
    case DRUM_METAL: {
        const uint16_t *r = metal_ratios[(e[MT_COLOR] >> 5) & 3];
        for (unsigned i = 0; i < 6; i++)
            x.incs[i] = (uint32_t)(((uint64_t)x.inc * r[i]) >> 8);
        x.k = table_svf[clamp127(e[MT_TONE])];
        x.a = 2 * 16384 - (int32_t)clamp127(e[MT_RING]) * 250;  /* SVF damping Q14 */
        if (x.a < 600) x.a = 600;
        x.mode = e[MT_REVERSE] ? 1 : 0;
        if (x.mode) {
            uint32_t steps = 1u + clamp127(e[MT_REVERSE]) * 16u / 128u;
            x.length = steps * step_frames(d);
            if (x.length < 64) x.length = 64;
            x.ramp_inc = Q30 / x.length;
        }
        break;
    }
    case DRUM_SHAKER: {
        x.k = table_svf[clamp127(e[SH_TUNE])];
        x.b = table_svf[clamp127(e[SH_TUNE2])];
        x.a = 2 * 16384 - (int32_t)clamp127(e[SH_RESO]) * 250;
        if (x.a < 600) x.a = 600;
        x.mode = (int32_t)clamp127(e[SH_BEADS]) * clamp127(e[SH_BEADS]) / 4 + 8; /* /65536 */
        x.coef2 = table_decay[clamp127(e[SH_BDEC]) / 3];
        x.noise_level = (int32_t)clamp127(e[SH_MIX]) * 258;
        break;
    }
    case DRUM_OBJECT: {
        unsigned m = clamp127(e[OB_MATERIAL]) * 5u / 128u;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t inc = (uint32_t)(((uint64_t)x.inc * object_ratios[m][i]) >> 8);
            if (inc > 0x30000000u) { x.res_gain[i] = 0; continue; }   /* above ~8 kHz */
            int di = (int)clamp127(e[OB_DECAY]) + object_damp[m][i];
            uint32_t rr = table_decay[di < 0 ? 0 : di];           /* r, Q30 */
            int64_t cw = table_sine[((inc >> 24) + 64) & 255];    /* cos w, Q15 */
            int64_t sw = table_sine[(inc >> 24) & 255];           /* sin w, Q15 */
            x.res_b1[i] = (int32_t)((2 * (int64_t)rr * cw) >> 15);
            x.res_b2[i] = (int32_t)(((int64_t)rr * rr) >> 30);
            int64_t g = (((int64_t)(Q30 - rr) >> 15) * sw * 2) >> 15;   /* (1-r) sin w */
            x.res_gain[i] = (int32_t)(g > 32767 ? 32767 : g < 1 ? 1 : g) * (4 - (int)i) / 4;
        }
        unsigned len = 2u + (127u - clamp127(e[OB_HARD])) / 4u;   /* mallet, frames */
        x.mode = (int32_t)len;
        x.incs[0] = 0x80000000u / len;                            /* half-sine pulse */
        x.k = (int32_t)clamp127(e[OB_CRUNCH]) * clamp127(e[OB_CRUNCH]) / 2;  /* /65536 */
        x.coef3 = table_decay[clamp127(e[OB_CDEC])];
        x.b = (int32_t)clamp127(e[OB_RATTLE]) * 258;
        break;
    }
    case DRUM_NOISE: {
        x.mode = (int32_t)(clamp127(e[NZ_MODE]) * 3u / 128u);    /* rise, fall, impact */
        uint32_t steps = 1u + clamp127(e[NZ_LENGTH]) / 2u;
        x.length = steps * step_frames(d);
        if (x.length < 256) x.length = 256;
        x.ramp_inc = Q30 / x.length;
        x.k = (int32_t)clamp127(e[NZ_TUNE]);                     /* start index */
        x.b = ((int32_t)clamp127(e[NZ_RANGE]) - 64) * 2;          /* index span */
        x.a = 2 * 16384 - (int32_t)clamp127(e[NZ_RESO]) * 250;
        if (x.a < 600) x.a = 600;
        x.noise_level = (int32_t)clamp127(e[NZ_SUB]) * 258;
        x.coef2 = table_decay[50];                                /* sub pitch drop */
        x.coef3 = table_decay[45];                                /* impact burst */
        x.incs[0] = table_tune[30];                               /* sub ~60 Hz */
        x.sweep = table_tune[50];
        break;
    }
    default:
        break;
    }

    /* Common chain. */
    const uint8_t *c = p->c;
    x.ftype = (uint8_t)(clamp127(c[DC_FTYPE]) >> 5);
    x.cutoff = (uint8_t)clamp127(c[DC_CUTOFF]);
    x.fenv = (int8_t)((int)clamp127(c[DC_FENV]) - 64);
    x.fenv_coef = table_decay[clamp127(c[DC_FDECAY])];
    x.fq = 2 * 16384 - (int32_t)clamp127(c[DC_RESO]) * 250;
    if (x.fq < 600) x.fq = 600;
    x.drive = 256 + (int32_t)clamp127(c[DC_DRIVE]) * 14;          /* 1x .. ~8x */
    x.crush_shift = (uint8_t)((127u - clamp127(c[DC_BITS])) * 12u / 127u);   /* 16 .. 4 bits */
    x.hold = (uint8_t)(1u + clamp127(c[DC_RATE]) / 4u);           /* 1 .. 32 frames */
    x.trans = (uint8_t)(clamp127(c[DC_TRANS]) >> 5);
    x.trans_level = (int32_t)c[DC_TLEVEL] * 258;
    x.trans_coef = table_decay[x.trans == 3 ? 24 : x.trans == 2 ? 12 : 4];
    x.texture = (uint8_t)(clamp127(c[DC_TEXTURE]) >> 5);
    x.tex_level = (int32_t)c[DC_XLEVEL] * 258;
    x.level = (int32_t)c[DC_LEVEL] * 258;
    unsigned pan = clamp127(c[DC_PAN]);
    x.pan_l = pan <= 64 ? 32767 : (int32_t)((127 - pan) * 32767 / 63);
    x.pan_r = pan >= 64 ? 32767 : (int32_t)(pan * 32767 / 64);
    d->derived[v] = x;
}

void drums_prepare(struct drums *d, const struct drum_kit *kit)
{
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        drums_prepare_voice(d, kit, v);
}

void drums_set_tempo(struct drums *d, const struct drum_kit *kit, unsigned bpm100)
{
    if (bpm100 < SEQ_BPM_MIN) bpm100 = SEQ_BPM_MIN;
    /* frames per 1/16 step = 44100 * 60 / (bpm * 4), in Q8 */
    d->step_frames_q8 = (uint32_t)(((uint64_t)RATE * 60u * 100u * 256u) / ((uint64_t)bpm100 * 4u));
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        if (kit->voice[v].engine == DRUM_METAL || kit->voice[v].engine == DRUM_NOISE)
            drums_prepare_voice(d, kit, v);
}

void drums_init(struct drums *d)
{
    memset(d, 0, sizeof *d);
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        d->voice[v].noise = 0x9E3779B9u ^ (v * 0x85EBCA6Bu);
    d->step_frames_q8 = 5512u * 256u;   /* 120 BPM until told otherwise */
}

/* --- audio interrupt ------------------------------------------------------- */

FM1_AUDIO_CODE static inline int32_t sine(uint32_t phase)
{
    uint32_t i = phase >> 24, frac = (phase >> 8) & 0xFFFFu;
    int32_t a = table_sine[i], b = table_sine[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)frac) >> 16);
}

FM1_AUDIO_CODE static inline uint32_t decay(uint32_t env, uint32_t coef)
{
    return (uint32_t)(((uint64_t)env * coef) >> 30);
}

FM1_AUDIO_CODE static inline uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

FM1_AUDIO_CODE static inline int32_t clamp15(int32_t y)
{
    return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

/* Chamberlin state-variable filter, two passes per sample (2x), f and q Q14.
 * Returns low, band or high per type (1, 3, 2). */
FM1_AUDIO_CODE static inline int32_t svf2x(int32_t *low, int32_t *band, int32_t x,
                                           int32_t f, int32_t q, unsigned type)
{
    int32_t high = 0;
    for (unsigned i = 0; i < 2; i++) {
        *low += (f * *band) >> 14;
        high = x - *low - ((q * *band) >> 14);
        *band += (f * high) >> 14;
        if (*band > (1 << 20)) *band = 1 << 20;
        if (*band < -(1 << 20)) *band = -(1 << 20);
        if (*low > (1 << 20)) *low = 1 << 20;
        if (*low < -(1 << 20)) *low = -(1 << 20);
    }
    return type == 1 ? *low : type == 2 ? high : *band;
}

/* Soft clip in Q15: cubic on [-1.5, 1.5]. */
FM1_AUDIO_CODE static inline int32_t soft15(int32_t x)
{
    if (x > 49152) x = 49152;
    if (x < -49152) x = -49152;
    int32_t x3 = (int32_t)((((int64_t)x * x >> 15) * x) >> 15);
    return clamp15(x - ((x3 * 9709) >> 16));      /* x - 4/27 x^3 */
}

FM1_AUDIO_CODE static inline int32_t white(uint32_t *s)
{
    return (int32_t)(int16_t)(rnd(s) >> 16);
}

FM1_AUDIO_CODE void drums_trigger(struct drums *d, unsigned v, unsigned velocity)
{
    if (v >= DRUM_VOICES)
        return;
    const struct drum_derived *x = &d->derived[v];
    if (x->engine == DRUM_OFF)
        return;
    if (x->engine == DRUM_SAMPLE && !d->sample.pcm)
        return;
    if (x->choke)
        for (unsigned o = 0; o < DRUM_VOICES; o++)
            if (o != v && d->voice[o].active && d->derived[o].choke == x->choke &&
                !d->voice[o].fade)
                d->voice[o].fade = CHOKE_FADE;
    struct drum_voice_state *s = &d->voice[v];
    /* Field by field: a struct clear could become a memset call (flash). */
    if (!s->noise) s->noise = 1;
    s->fade = 0;
    for (unsigned i = 0; i < 6; i++) s->phase[i] = 0;
    s->s0 = s->s1 = s->s2 = s->s3 = 0;
    s->env = s->env2 = s->env3 = s->env4 = Q30;
    s->pos = 0;
    s->count = 0;
    for (unsigned i = 0; i < 4; i++) s->ry1[i] = s->ry2[i] = 0;
    s->fenv = Q30;
    s->low = s->band = 0;
    s->held = 0; s->hold_count = 0;
    s->tenv = x->trans ? Q30 : 0;
    s->tphase = 0;
    s->xenv = Q30;
    s->xlp = 0;
    if (velocity > 127) velocity = 127;
    s->gain = (int32_t)(((int64_t)x->level * (int32_t)velocity * 516) >> 16);

    if (x->engine == DRUM_KS) {
        /* Excite the delay line: one period of noise, a click, the kick
         * sample or a mallet pulse, through a one-pole low-pass. */
        int16_t *buf = d->ks[v];
        uint32_t n = x->period_q16 >> 16;
        int32_t lp = 0;
        for (uint32_t i = 0; i < DRUM_KS_LEN; i++) {
            int32_t e = 0;
            if (i <= n) {
                switch (x->mode) {
                case 0: e = white(&s->noise); break;
                case 1: e = i < 3 ? 30000 : 0; break;
                case 2: e = d->sample.pcm && i < d->sample.frames ? d->sample.pcm[i] : white(&s->noise); break;
                default: e = i < 24 ? sine(i * (0x80000000u / 24u)) : 0; break;
                }
            }
            lp += ((e - lp) * x->noise_a) >> 15;
            buf[i] = (int16_t)clamp15(lp);
        }
        s->pos = n + 1;   /* write index */
    }
    s->active = 1;
}

/* One sample of the voice's engine, Q15, or 0x7FFFFFFF when it has ended. */
#define ENDED 0x7FFFFFFF
FM1_AUDIO_CODE static int32_t engine_sample(struct drums *d, unsigned v,
                                            struct drum_voice_state *s,
                                            const struct drum_derived *x)
{
    int32_t y = 0;
    switch (x->engine) {
    case DRUM_SAMPLE: {
        uint32_t i = s->pos >> 16;
        if (!d->sample.pcm || i + 1 >= d->sample.frames) return ENDED;
        int32_t a = d->sample.pcm[i], b = d->sample.pcm[i + 1];
        y = a + (((b - a) * (int32_t)((s->pos & 0xFFFFu) >> 1)) >> 15);
        s->pos += x->sample_step;
        y = (int32_t)(((int64_t)y * (int32_t)(s->env >> 15)) >> 15);
        s->env = decay(s->env, x->amp_coef);
        return y;
    }
    case DRUM_FM: {
        uint32_t inc = x->inc + (uint32_t)(((uint64_t)x->sweep * (s->env2 >> 14)) >> 16);
        uint32_t minc = (uint32_t)(((uint64_t)inc * x->ratio_q8) >> 8);
        int32_t m = sine(s->phase[1] + (uint32_t)(s->s0 * x->b));
        s->s0 = m;
        s->phase[1] += minc;
        uint32_t off = (uint32_t)(((int64_t)m * x->a * (int32_t)(s->env3 >> 15)) >> 15);
        int32_t c = sine(s->phase[0] + off);
        s->phase[0] += inc;
        y = (c * (int32_t)(s->env >> 15)) >> 15;
        int32_t w = white(&s->noise);
        s->s1 += ((w - s->s1) * x->noise_a) >> 15;
        int32_t f = x->noise_highpass ? w - s->s1 : s->s1;
        y += (int32_t)(((int64_t)f * (int32_t)(s->env4 >> 15) >> 15) * x->noise_level >> 15);
        s->env = decay(s->env, x->amp_coef);
        s->env2 = decay(s->env2, x->coef2);
        s->env3 = decay(s->env3, x->coef3);
        s->env4 = decay(s->env4, x->coef4);
        if (s->env < 64 && s->env4 < 64) return ENDED;
        return y;
    }
    case DRUM_KS: {
        int16_t *buf = d->ks[v];
        uint32_t period = x->period_q16 + (uint32_t)(((uint64_t)x->sweep * (s->env2 >> 14)) >> 16);
        uint32_t w = s->pos;
        uint32_t rd = (w << 16) - period;                 /* Q16 read position */
        uint32_t i0 = (rd >> 16) & (DRUM_KS_LEN - 1), i1 = (i0 + 1) & (DRUM_KS_LEN - 1);
        int32_t frac = (int32_t)((rd & 0xFFFFu) >> 1);
        int32_t a = buf[i0] + (((buf[i1] - buf[i0]) * frac) >> 15);
        /* Loop filter: two-point average (k), dispersion allpass (a), gain. */
        int32_t lp = (a * (32768 - x->k) + s->s0 * x->k) >> 15;
        s->s0 = a;
        int32_t ap = ((x->a * (lp - s->s2)) >> 15) + s->s1;
        s->s1 = lp;
        s->s2 = ap;
        int32_t fb = (ap * x->loop_gain) >> 15;
        buf[w & (DRUM_KS_LEN - 1)] = (int16_t)clamp15(fb);
        s->pos = w + 1;
        y = a;
        y += (int32_t)(((int64_t)white(&s->noise) * (int32_t)(s->env4 >> 15) >> 15) * x->noise_level >> 15);
        s->env2 = decay(s->env2, x->coef2);
        s->env4 = decay(s->env4, x->coef4);
        s->env = decay(s->env, x->amp_coef);              /* only to end the voice */
        if (++s->count > 4 * RATE) return ENDED;
        if (s->env < 2000 && s->count > 512 && (a < 4 && a > -4)) return ENDED;
        return y;
    }
    case DRUM_METAL: {
        int32_t sum = 0;
        for (unsigned i = 0; i < 6; i++) {
            s->phase[i] += x->incs[i];
            sum += (s->phase[i] & 0x80000000u) ? -5000 : 5000;
        }
        int32_t band = svf2x(&s->s0, &s->s1, sum, x->k, x->a, 3);
        int32_t hp = band - s->s2;                         /* remove low rumble */
        s->s2 += (band - s->s2) >> 6;
        uint32_t amp;
        if (x->mode) {                                     /* reverse: swell, then cut */
            if (s->count >= x->length) {
                if (s->count >= x->length + 64) return ENDED;
                amp = (uint32_t)((uint64_t)Q30 * (x->length + 64 - s->count) >> 6);
            } else {
                uint32_t r = s->pos;                       /* ramp, Q30 */
                amp = (uint32_t)(((uint64_t)r * r) >> 30);
                s->pos = r + x->ramp_inc;
            }
        } else {
            amp = s->env;
            s->env = decay(s->env, x->amp_coef);
            if (s->env < 64) return ENDED;
        }
        s->count++;
        return clamp15((int32_t)(((int64_t)hp * (int32_t)(amp >> 15)) >> 14));
    }
    case DRUM_SHAKER: {
        /* Particle model: beads collide with a probability that follows the
         * shake energy; each collision feeds a fast-decaying sound level
         * that gates noise into two shell resonances. */
        uint32_t r = rnd(&s->noise);
        uint32_t p = (uint32_t)(((uint64_t)x->mode * (s->env >> 14)) >> 16);
        if ((r & 0xFFFFu) < p)
            s->env2 += (s->env >> 2) & 0x3FFFFFFFu;
        if (s->env2 > Q30) s->env2 = Q30;
        int32_t n = (int32_t)(((int64_t)(int16_t)(r >> 16) * (int32_t)(s->env2 >> 15)) >> 15);
        int32_t a = svf2x(&s->s0, &s->s1, n, x->k, x->a, 3);
        int32_t b = svf2x(&s->s2, &s->s3, n, x->b, x->a, 3);
        y = a + (((b - a) * x->noise_level) >> 15);
        s->env2 = decay(s->env2, x->coef2);
        s->env = decay(s->env, x->amp_coef);
        if (s->env < 64 && s->env2 < 64) return ENDED;
        return clamp15(y * 2);
    }
    case DRUM_OBJECT: {
        int32_t in = 0;
        if (s->count < (uint32_t)x->mode)                  /* mallet: half-sine pulse */
            in = sine(s->count * x->incs[0]);
        uint32_t r = rnd(&s->noise);
        uint32_t p = (uint32_t)(((uint64_t)x->k * (s->env3 >> 14)) >> 16);
        if ((r & 0xFFFFu) < p)                             /* crunch grains */
            in += (int32_t)(int16_t)(r >> 16);
        s->env3 = decay(s->env3, x->coef3);
        int32_t sum = 0;
        for (unsigned i = 0; i < 4; i++) {
            if (!x->res_gain[i]) continue;
            int64_t yy = (((int64_t)x->res_b1[i] * s->ry1[i]) >> 30)
                       - (((int64_t)x->res_b2[i] * s->ry2[i]) >> 30)
                       + (((int64_t)in * x->res_gain[i]) >> 12);
            if (yy > (1 << 22)) yy = 1 << 22;
            if (yy < -(1 << 22)) yy = -(1 << 22);
            s->ry2[i] = s->ry1[i];
            s->ry1[i] = (int32_t)yy;
            sum += (int32_t)yy;
        }
        y = clamp15(sum);
        if (x->b) {                                        /* rattle: buzz when loud */
            int32_t mag = y < 0 ? -y : y;
            if (mag > 6000)
                y += (int32_t)(((int64_t)(int16_t)(r >> 8) * x->b) >> 15) * (mag >> 12) >> 3;
        }
        s->count++;
        s->env = decay(s->env, x->amp_coef);               /* end detection only */
        if (s->count > 4 * RATE) return ENDED;
        if (s->count > (uint32_t)x->mode + 2048 && s->env < 4000 && s->env3 < 4000) {
            int32_t mag = y < 0 ? -y : y;
            if (mag < 8) return ENDED;
        }
        return clamp15(y);
    }
    case DRUM_NOISE: {
        int32_t w = white(&s->noise);
        if (x->mode == 2) {                                 /* impact */
            uint32_t inc = x->incs[0] + (uint32_t)(((uint64_t)x->sweep * (s->env2 >> 14)) >> 16);
            int32_t sub = sine(s->phase[0]);
            s->phase[0] += inc;
            int32_t burst = svf2x(&s->s0, &s->s1, w, table_svf[x->k], x->a, 3);
            y = ((sub * x->noise_level) >> 15) + (int32_t)(((int64_t)burst * (int32_t)(s->env3 >> 15)) >> 15);
            y = (int32_t)(((int64_t)y * (int32_t)(s->env >> 15)) >> 15);
            s->env = decay(s->env, x->amp_coef);
            s->env2 = decay(s->env2, x->coef2);
            s->env3 = decay(s->env3, x->coef3);
            if (s->env < 64) return ENDED;
            return clamp15(y);
        }
        if (s->count >= x->length) return ENDED;
        uint32_t ramp = s->pos;                             /* 0 .. Q30 */
        s->pos += x->ramp_inc;
        s->count++;
        uint32_t t = x->mode == 0 ? ramp : Q30 - ramp;
        int32_t idx = x->k + (int32_t)(((int64_t)x->b * (int32_t)(t >> 15)) >> 15);
        idx = idx < 0 ? 0 : idx > 127 ? 127 : idx;
        int32_t band = svf2x(&s->s0, &s->s1, w, table_svf[idx], x->a, 3);
        uint32_t amp = (uint32_t)(((uint64_t)t * t) >> 30);
        return clamp15((int32_t)(((int64_t)band * (int32_t)(amp >> 15)) >> 13));
    }
    default:
        return ENDED;
    }
}

/* Transient layer sample (Q15) for this frame. */
FM1_AUDIO_CODE static inline int32_t transient(struct drum_voice_state *s,
                                               const struct drum_derived *x)
{
    if (!x->trans || s->tenv < 64) return 0;
    int32_t t;
    switch (x->trans) {
    case 1: t = s->count < 2 ? 32767 : s->count < 5 ? -16000 : 0; break;        /* click */
    case 2: { int32_t w = white(&s->noise); s->xlp += (w - s->xlp) >> 2; t = w - s->xlp; break; }  /* snap */
    default: t = sine(s->tphase); s->tphase += table_tune[26]; break;          /* thump ~55 Hz */
    }
    t = (int32_t)(((int64_t)t * (int32_t)(s->tenv >> 15)) >> 15);
    s->tenv = decay(s->tenv, x->trans_coef);
    return (t * x->trans_level) >> 15;
}

FM1_AUDIO_CODE static void render_voice(struct drums *d, unsigned v, int32_t *out,
                                        unsigned from, unsigned to)
{
    struct drum_voice_state *s = &d->voice[v];
    const struct drum_derived *x = &d->derived[v];
    for (unsigned f = from; f < to && s->active; f++) {
        int32_t y = engine_sample(d, v, s, x);
        if (y == ENDED) { s->active = 0; break; }
        y += transient(s, x);

        if (x->drive > 256)
            y = soft15((y * x->drive) >> 8);
        if (x->ftype) {
            int32_t idx = x->cutoff + (int32_t)((x->fenv * (int32_t)(s->fenv >> 15)) >> 15);
            idx = idx < 0 ? 0 : idx > 127 ? 127 : idx;
            y = svf2x(&s->low, &s->band, y, table_svf[idx], x->fq, x->ftype);
            s->fenv = decay(s->fenv, x->fenv_coef);
        }
        if (x->crush_shift)
            y &= ~((1 << x->crush_shift) - 1);
        if (x->hold > 1) {
            if (s->hold_count == 0) s->held = y;
            if (++s->hold_count >= x->hold) s->hold_count = 0;
            y = s->held;
        }
        if (x->texture) {
            uint32_t r = rnd(&s->noise);
            int32_t t = 0;
            if ((x->texture & 1) && (r & 0x7FFu) < 2)        /* crackle */
                t += (int32_t)(int16_t)(r >> 16);
            if (x->texture & 2) {                            /* hiss */
                int32_t w = (int32_t)(int16_t)(r >> 8);
                s->xlp += (w - s->xlp) >> 1;
                t += (w - s->xlp) >> 2;
            }
            y += (int32_t)((((int64_t)t * (int32_t)(s->xenv >> 15)) >> 15) * x->tex_level >> 15);
            s->xenv = decay(s->xenv, x->amp_coef);
        }

        y = clamp15(y);
        y = (y * s->gain) >> 15;
        if (s->fade) {
            y = (y * (int32_t)s->fade) >> 6;  /* / CHOKE_FADE (64) */
            if (--s->fade == 0) s->active = 0;
        }
        out[2 * f] += ((y * x->pan_l) >> 15) * 256;      /* Q15 -> Q23 */
        out[2 * f + 1] += ((y * x->pan_r) >> 15) * 256;
    }
}

FM1_AUDIO_CODE void drums_render(struct drums *d, int32_t *out, unsigned frames,
                                 const struct seq_event *ev, unsigned n)
{
    unsigned at = 0;
    for (unsigned e = 0; e <= n; e++) {
        unsigned until = e < n ? ev[e].frame : frames;
        if (until > frames) until = frames;
        if (until > at) {
            for (unsigned v = 0; v < DRUM_VOICES; v++)
                if (d->voice[v].active)
                    render_voice(d, v, out, at, until);
            at = until;
        }
        if (e < n)
            drums_trigger(d, ev[e].voice, ev[e].velocity);
    }
}
