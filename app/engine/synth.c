/* SPDX-License-Identifier: MIT */
#include "synth.h"

#include <string.h>

#include "dsp.h"
#include "fm1.h"
#include "tables.h"

#define Q30 (1u << 30)
#define ENV_END (1u << 16)        /* release ends below ~-84 dB */

/* --- names ----------------------------------------------------------------- */

static const char *const engine_names[SY_ENGINES] = {
    "OFF", "808 BASS", "FM LEAD", "KOTO", "FLUTE", "VIBES", "PAD",
};
static const char *const engine_params[SY_ENGINES][SYNTH_EP] = {
    [SY_BASS] = { "TUNE", "P.ENV", "P.DEC", "DRIVE", "FOLD", "TONE", "CLICK", "SUB" },
    [SY_LEAD] = { "TUNE", "RATIO", "MOD", "M.DEC", "M.SUS", "FDBK", "V.RT", "V.DEP" },
    [SY_KOTO] = { "TUNE", "PLUCK", "DAMP", "DECAY", "BRITE", "BODY" },
    [SY_FLUTE] = { "TUNE", "BRETH", "TONE", "CHIFF", "V.RT", "V.DEP", "AIR" },
    [SY_VIBES] = { "TUNE", "HARD", "DECAY", "TONE2", "TONE3", "T.RT", "T.DEP" },
    [SY_PAD] = { "TUNE", "BRITE", "RESO", "SPRED", "AIR" },
};
static const char *const common_names[SC_COUNT] = {
    "LEVEL", "PAN", "ATK", "DEC", "SUS", "REL", "F.TYP", "CUTOF", "RESO",
    "F.ENV", "F.DEC", "DRIVE", "GLIDE",
};
static const char *const mode_names[SM_MODES] = { "POLY", "MONO", "LEGAT" };

const char *synth_engine_name(unsigned e) { return e < SY_ENGINES ? engine_names[e] : "?"; }
const char *synth_engine_param_name(unsigned e, unsigned i)
{
    return e < SY_ENGINES && i < SYNTH_EP ? engine_params[e][i] : 0;
}
const char *synth_common_name(unsigned i) { return i < SC_COUNT ? common_names[i] : "?"; }
const char *synth_mode_name(unsigned m) { return m < SM_MODES ? mode_names[m] : "?"; }

/* --- patch ----------------------------------------------------------------- */

void synth_layer_engine_defaults(struct synth_layer_params *l, unsigned engine)
{
    /* engine params, then ADSR, filter type/cutoff, mode, glide */
    static const struct {
        uint8_t e[SYNTH_EP];
        uint8_t a, d, s, r, ftype, cutoff, mode, glide;
    } def[SY_ENGINES] = {
        [SY_OFF] = { { 64 }, 0, 64, 127, 40, 0, 127, SM_POLY, 0 },
        [SY_BASS] = { { 64, 40, 30, 30, 0, 90, 30, 0 }, 0, 90, 0, 40, 0, 127, SM_LEGATO, 20 },
        [SY_LEAD] = { { 64, 12, 40, 60, 30, 0, 50, 10 }, 5, 60, 90, 40, 48, 110, SM_POLY, 0 },
        [SY_KOTO] = { { 64, 30, 40, 100, 90, 40 }, 0, 127, 127, 30, 0, 127, SM_POLY, 0 },
        [SY_FLUTE] = { { 64, 40, 30, 50, 55, 20, 20 }, 30, 60, 110, 45, 48, 120, SM_LEGATO, 10 },
        [SY_VIBES] = { { 64, 70, 122, 50, 20, 50, 40 }, 0, 127, 127, 60, 0, 127, SM_POLY, 0 },
        [SY_PAD] = { { 64, 60, 90, 40, 10 }, 80, 80, 100, 90, 48, 100, SM_POLY, 0 },
    };
    if (engine >= SY_ENGINES) engine = SY_OFF;
    l->engine = (uint8_t)engine;
    memcpy(l->e, def[engine].e, sizeof l->e);
    l->c[SC_ATTACK] = def[engine].a;
    l->c[SC_DECAY] = def[engine].d;
    l->c[SC_SUSTAIN] = def[engine].s;
    l->c[SC_RELEASE] = def[engine].r;
    l->c[SC_FTYPE] = def[engine].ftype;
    l->c[SC_CUTOFF] = def[engine].cutoff;
    l->c[SC_GLIDE] = def[engine].glide;
    l->mode = def[engine].mode;
}

void synth_patch_default(struct synth_patch *p)
{
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < SYNTH_LAYERS; i++) {
        struct synth_layer_params *l = &p->layer[i];
        l->lo = 0;
        l->hi = 127;
        l->octave = 0;
        l->c[SC_LEVEL] = 100;
        l->c[SC_PAN] = 64;
        l->c[SC_RESO] = 0;
        l->c[SC_FENV] = 64;
        l->c[SC_FDECAY] = 50;
        l->c[SC_DRIVE] = 0;
        synth_layer_engine_defaults(l, i == 0 ? SY_LEAD : SY_OFF);
    }
}

/* --- prepare (control task) ------------------------------------------------ */

static unsigned clamp127(int v) { return v < 0 ? 0u : v > 127 ? 127u : (unsigned)v; }

void synth_prepare_layer(struct synth *s, const struct synth_patch *patch, unsigned li)
{
    const struct synth_layer_params *p = &patch->layer[li];
    struct synth_derived x;
    memset(&x, 0, sizeof x);
    x.engine = p->engine < SY_ENGINES ? p->engine : SY_OFF;
    x.mode = p->mode < SM_MODES ? p->mode : SM_POLY;
    x.lo = p->lo;
    x.hi = p->hi;
    int oct = p->octave < -3 ? -3 : p->octave > 3 ? 3 : p->octave;
    x.transpose = (int8_t)(oct * 12);
    const uint8_t *e = p->e, *c = p->c;
    x.fine_q16 = table_fine[clamp127(e[SE_TUNE]) * 128u / 127u];
    x.k = (int32_t)(clamp127(e[SE_TUNE]) * 128u / 127u);       /* fine index (KOTO) */

    /* ADSR: attack 1 ms .. 5 s (cubic), decay/release 3 ms .. 3 s to -60 dB. */
    unsigned av = clamp127(c[SC_ATTACK]);
    uint32_t frames = 44u + (uint32_t)((uint64_t)av * av * av * 220500u / 2048383u);
    x.attack_inc = Q30 / frames;
    x.decay_coef = table_decay[clamp127(c[SC_DECAY])];
    x.sustain = clamp127(c[SC_SUSTAIN]) * (Q30 / 127u);
    x.release_coef = table_decay[clamp127(c[SC_RELEASE])];

    x.ftype = (uint8_t)(clamp127(c[SC_FTYPE]) >> 5);
    x.cutoff = (uint8_t)clamp127(c[SC_CUTOFF]);
    x.fenv = (int8_t)((int)clamp127(c[SC_FENV]) - 64);
    x.fenv_coef = table_decay[clamp127(c[SC_FDECAY])];
    x.fq = 2 * 16384 - (int32_t)clamp127(c[SC_RESO]) * 250;
    if (x.fq < 600) x.fq = 600;
    x.drive = 256 + (int32_t)clamp127(c[SC_DRIVE]) * 14;
    unsigned g = clamp127(c[SC_GLIDE]);
    x.glide_k = g ? (Q30 - table_decay[g]) >> 14 : 65536u;
    if (!x.glide_k) x.glide_k = 1;
    x.level = (int32_t)clamp127(c[SC_LEVEL]) * 258;
    unsigned pan = clamp127(c[SC_PAN]);
    x.pan_l = pan <= 64 ? 32767 : (int32_t)((127 - pan) * 32767 / 63);
    x.pan_r = pan >= 64 ? 32767 : (int32_t)(pan * 32767 / 64);

    switch (x.engine) {
    case SY_BASS:
        x.a = (int32_t)clamp127(e[BA_PENV]) * 774;         /* pitch env: up to +3x inc */
        x.coef1 = table_decay[clamp127(e[BA_PDEC]) / 2];
        x.coef2 = table_decay[6];                          /* click */
        x.b = 256 + (int32_t)clamp127(e[BA_DRIVE]) * 14;   /* Q8 */
        x.c = e[BA_FOLD] ? 256 + (int32_t)clamp127(e[BA_FOLD]) * 24 : 0;   /* Q8 fold gain */
        x.d = (int32_t)(clamp127(e[BA_TONE]) + 1) * 256;  /* one-pole low-pass */
        x.gain[0] = (int32_t)clamp127(e[BA_CLICK]) * 258;
        x.gain[1] = (int32_t)clamp127(e[BA_SUB]) * 258;
        break;
    case SY_LEAD:
        x.ratio_q8[0] = (uint32_t)table_ratio[clamp127(e[LE_RATIO]) * 16u / 128u] << 8;  /* Q16 */
        x.a = (int32_t)clamp127(e[LE_MOD]) * 1314;         /* index up to ~8 rad */
        x.coef1 = table_decay[clamp127(e[LE_MDEC])];
        x.b = (int32_t)(clamp127(e[LE_MSUS]) * (Q30 / 127u));   /* mod sustain, Q30 */
        x.c = (int32_t)clamp127(e[LE_FDBK]) * 246;         /* up to ~1.5 rad */
        x.lfo_inc = 9739u + clamp127(e[LE_VRATE]) * 9126u; /* 0.1 .. 12 Hz */
        x.d = (int32_t)clamp127(e[LE_VDEPTH]) * 16;        /* up to ~6 % of pitch */
        break;
    case SY_KOTO:
        x.a = 1024 + (int32_t)clamp127(e[KO_PLUCK]) * 121;  /* pluck point, Q15 of period */
        x.b = 32767 - (int32_t)clamp127(e[KO_DAMP]) * 129;  /* loop low-pass weight */
        x.c = 31500 + (int32_t)clamp127(e[KO_DECAY]) * 10;  /* loop gain per pass */
        if (x.c > 32760) x.c = 32760;
        x.d = (int32_t)(clamp127(e[KO_BRIGHT]) + 1) * 256; /* excitation low-pass */
        x.gain[0] = (int32_t)clamp127(e[KO_BODY]) * 258;
        break;
    case SY_FLUTE:
        x.a = (int32_t)clamp127(e[FL_BREATH]) * 258;
        x.b = (int32_t)clamp127(e[FL_TONE]) * 258;
        x.c = (int32_t)clamp127(e[FL_CHIFF]) * 258;
        x.coef2 = table_decay[14];                          /* chiff burst */
        x.lfo_inc = 9739u + clamp127(e[FL_VRATE]) * 9126u;
        x.d = (int32_t)clamp127(e[FL_VDEPTH]) * 16;
        x.gain[0] = (int32_t)clamp127(e[FL_AIR]) * 130;
        break;
    case SY_VIBES: {
        static const uint32_t ratios[3] = { 1u << 16, 4u << 16, 10u << 16 };   /* tuned bar */
        static const int8_t damp[3] = { 0, -14, -28 };
        for (unsigned i = 0; i < 3; i++) {
            int di = (int)clamp127(e[VI_DECAY]) + damp[i];
            x.ratio_q8[i] = ratios[i];
            x.r_q30[i] = table_decay[di < 0 ? 0 : di];
        }
        x.gain[0] = 32767;
        x.gain[1] = (int32_t)clamp127(e[VI_TONE2]) * 258;
        x.gain[2] = (int32_t)clamp127(e[VI_TONE3]) * 258;
        unsigned len = 2u + (127u - clamp127(e[VI_HARD])) / 3u;   /* mallet, frames */
        x.b = (int32_t)(0x80000000u / len);                      /* half-sine pulse */
        x.c = (int32_t)(25600u / len);                            /* excitation scale */
        x.lfo_inc = 48696u + clamp127(e[VI_TRATE]) * 7300u;      /* 0.5 .. 10 Hz */
        x.a = (int32_t)clamp127(e[VI_TDEPTH]) * 200;
        break;
    }
    case SY_PAD: {
        static const int8_t detune[4] = { 0, 1, -1, 2 };
        unsigned spread = clamp127(e[PA_SPREAD]);
        for (unsigned i = 0; i < 4; i++)                          /* partials 1..4, spread */
            x.ratio_q8[i] = (uint32_t)(((uint64_t)((i + 1u) << 16) *
                                        (uint32_t)(65536 + detune[i] * (int)spread * 8)) >> 16);
        x.a = 10294 - (int32_t)clamp127(e[PA_RESO]) * 79;        /* bandwidth, Q16 of pitch */
        int32_t bright = (int32_t)clamp127(e[PA_BRIGHT]) * 258;
        x.gain[0] = 32767;
        x.gain[1] = bright;
        x.gain[2] = (bright * bright) >> 15;
        x.gain[3] = (x.gain[2] * bright) >> 15;
        x.b = (int32_t)clamp127(e[PA_AIR]) * 130;
        break;
    }
    default:
        break;
    }
    s->derived[li] = x;
}

void synth_prepare(struct synth *s, const struct synth_patch *patch)
{
    for (unsigned i = 0; i < SYNTH_LAYERS; i++)
        synth_prepare_layer(s, patch, i);
}

void synth_init(struct synth *s)
{
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < SYNTH_VOICES; i++)
        s->voice[i].noise = 0x2545F491u ^ (i * 0x9E3779B9u);
}

/* --- audio interrupt ------------------------------------------------------- */

/* Integer square root (bit by bit, no division). */
FM1_AUDIO_CODE static uint32_t isqrt32(uint32_t n)
{
    uint32_t root = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

FM1_AUDIO_CODE static uint32_t note_inc(const struct synth_derived *x, unsigned note)
{
    return (uint32_t)(((uint64_t)table_note_inc[note] * x->fine_q16) >> 16);
}

/* One resonator: y = b1 y1 - b2 y2 + g x, coefficients Q30, gain Q15. */
FM1_AUDIO_CODE static void resonator_set(struct synth_voice *v, unsigned i, uint32_t inc,
                                         uint32_t r, int32_t gain)
{
    if (inc > 0x30000000u) { v->rg[i] = 0; v->rb1[i] = v->rb2[i] = 0; return; }   /* > ~8 kHz */
    /* cos w = 1 - 2 sin^2(w/2): keeps precision at low notes, where a Q15
     * cosine is too coarse (1 - cos w is ~5e-4 at 220 Hz). */
    int64_t sh = sine(inc >> 1);
    int64_t cw = (int64_t)Q30 - 2 * sh * sh;                 /* Q30 */
    v->rb1[i] = (int32_t)((2 * (int64_t)r * cw) >> 30);
    v->rb2[i] = (int32_t)(((int64_t)r * r) >> 30);
    v->rg[i] = gain;
    v->ry1[i] = v->ry2[i] = 0;
}

FM1_AUDIO_CODE static inline int32_t resonate(struct synth_voice *v, unsigned i, int32_t in)
{
    int64_t y = ((int64_t)v->rb1[i] * v->ry1[i] - (int64_t)v->rb2[i] * v->ry2[i]) >> 30;
    y += ((int64_t)in * v->rg[i]) >> 15;
    if (y > (1 << 20)) y = 1 << 20;
    if (y < -(1 << 20)) y = -(1 << 20);
    v->ry2[i] = v->ry1[i];
    v->ry1[i] = (int32_t)y;
    return (int32_t)y;
}

/* Note-on work that depends on the note: resonator coefficients, the KOTO
 * string. Table lookups and multiplies only. */
FM1_AUDIO_CODE static void engine_start(struct synth *s, unsigned vi, const struct synth_derived *x,
                                        unsigned note)
{
    struct synth_voice *v = &s->voice[vi];
    for (unsigned i = 0; i < 4; i++) v->phase[i] = 0;
    v->s0 = v->s1 = v->s2 = 0;
    v->env1 = v->env2 = Q30;
    v->fenv = Q30;
    v->low = v->band = 0;
    if (!v->noise) v->noise = 1;

    switch (x->engine) {
    case SY_LEAD:
        v->env1 = Q30;           /* mod envelope from the top */
        break;
    case SY_KOTO: {
        uint64_t period = ((uint64_t)table_note_period[note] * table_fine[128 - x->k]) >> 16;
        if (period > (uint64_t)(SYNTH_KS_LEN - 8) << 16) period = (uint64_t)(SYNTH_KS_LEN - 8) << 16;
        if (period < (4u << 16)) period = 4u << 16;
        v->period_q16 = (uint32_t)period - 32768u;          /* loop filter delay */
        int16_t *buf = s->ks[vi];
        uint32_t n = v->period_q16 >> 16;
        int32_t lp = 0;
        for (uint32_t i = 0; i < SYNTH_KS_LEN; i++) {
            int32_t e = i <= n ? white(&v->noise) : 0;
            lp += ((e - lp) * x->d) >> 15;
            buf[i] = (int16_t)clamp15(lp);
        }
        /* Pluck position: comb the excitation (backwards, in place). */
        uint32_t pp = (n * (uint32_t)x->a) >> 15;
        if (pp >= 1)
            for (uint32_t i = n; i >= pp; i--)
                buf[i] = (int16_t)((buf[i] - buf[i - pp]) >> 1);
        v->pos = n + 1;
        break;
    }
    case SY_VIBES: {
        uint32_t inc = note_inc(x, note);
        for (unsigned i = 0; i < 3; i++) {
            uint32_t mi = (uint32_t)(((uint64_t)inc * x->ratio_q8[i]) >> 16);
            int32_t sw = sine(mi);
            int32_t g = (int32_t)(((int64_t)sw * x->c) >> 15);
            resonator_set(v, i, mi, x->r_q30[i], (int32_t)(((int64_t)g * x->gain[i]) >> 15));
        }
        v->s1 = 1;               /* mallet pulse running */
        break;
    }
    case SY_PAD: {
        uint32_t inc = note_inc(x, note);
        for (unsigned i = 0; i < 4; i++) {
            uint32_t mi = (uint32_t)(((uint64_t)inc * x->ratio_q8[i]) >> 16);
            uint64_t one_minus_r = ((uint64_t)mi * (uint32_t)x->a) >> 16;    /* Q30 */
            if (one_minus_r > Q30 / 4) one_minus_r = Q30 / 4;
            if (one_minus_r < 64) one_minus_r = 64;
            uint32_t r = Q30 - (uint32_t)one_minus_r;
            /* Noise in, steady level out: g = 2 sin w sqrt(1 - r) */
            int32_t sw = sine(mi);
            int32_t sq = (int32_t)isqrt32((uint32_t)one_minus_r);           /* Q15 */
            int32_t g = (int32_t)(((int64_t)2 * sw * sq) >> 15);
            resonator_set(v, i, mi, r, (int32_t)(((int64_t)g * x->gain[i]) >> 15));
        }
        break;
    }
    case SY_FLUTE: {
        uint32_t inc = note_inc(x, note);
        v->s2 = sine(inc >> 2);   /* breath band-pass f at the note, Q14 */
        break;
    }
    default:
        break;
    }
}

FM1_AUDIO_CODE static unsigned alloc_voice(struct synth *s)
{
    unsigned best = 0;
    uint32_t best_env = 0xFFFFFFFFu;
    for (unsigned i = 0; i < SYNTH_VOICES; i++)
        if (!s->voice[i].active) return i;
    for (unsigned i = 0; i < SYNTH_VOICES; i++)           /* quietest released voice */
        if (!s->voice[i].gate && s->voice[i].env < best_env) { best_env = s->voice[i].env; best = i; }
    if (best_env != 0xFFFFFFFFu) return best;
    uint32_t oldest = 0;
    for (unsigned i = 0; i < SYNTH_VOICES; i++)           /* else the oldest note */
        if (s->clock - s->voice[i].age >= oldest) { oldest = s->clock - s->voice[i].age; best = i; }
    return best;
}

/* Start (or, legato, re-pitch) a voice on a note. */
FM1_AUDIO_CODE static void voice_note(struct synth *s, unsigned vi, unsigned layer, unsigned key,
                                      unsigned note, unsigned velocity, int retrigger)
{
    struct synth_voice *v = &s->voice[vi];
    const struct synth_derived *x = &s->derived[layer];
    uint32_t inc = note_inc(x, note);
    int fresh = !v->active || v->layer != layer;
    v->target = inc;
    if (fresh || x->mode == SM_POLY || x->glide_k >= 65536u) v->inc = inc;
    v->key = (uint8_t)key;
    if (!retrigger && !fresh) return;                  /* legato: glide only */
    v->layer = (uint8_t)layer;
    v->age = s->clock++;
    if (fresh) v->env = 0;
    v->stage = 0;                                      /* attack from where it is */
    v->gate = 1;
    if (velocity > 127) velocity = 127;
    v->vel = (int32_t)velocity * 258;
    engine_start(s, vi, x, note);
    v->active = 1;
}

FM1_AUDIO_CODE static unsigned layer_note(const struct synth_derived *x, unsigned key)
{
    int n = (int)key + x->transpose;
    return n < 0 ? 0u : n > 127 ? 127u : (unsigned)n;
}

FM1_AUDIO_CODE static int find_layer_voice(struct synth *s, unsigned layer)
{
    for (unsigned i = 0; i < SYNTH_VOICES; i++)
        if (s->voice[i].active && s->voice[i].layer == layer) return (int)i;
    return -1;
}

FM1_AUDIO_CODE void synth_note(struct synth *s, unsigned key, unsigned velocity)
{
    if (key > 127) return;
    for (unsigned l = 0; l < SYNTH_LAYERS; l++) {
        const struct synth_derived *x = &s->derived[l];
        if (x->engine == SY_OFF || key < x->lo || key > x->hi) continue;
        int mono = x->mode != SM_POLY;
        /* Plucked and struck engines retrigger even in legato mode. */
        int legato = x->mode == SM_LEGATO && x->engine != SY_KOTO && x->engine != SY_VIBES;

        if (velocity) {
            if (!mono) {
                voice_note(s, alloc_voice(s), l, key, layer_note(x, key), velocity, 1);
                continue;
            }
            /* Mono: last-note priority, held keys remembered. */
            uint8_t *held = s->held[l];
            unsigned n = s->held_n[l];
            for (unsigned i = 0; i < n; i++)          /* already held: move to top */
                if (held[i] == key) { for (; i + 1 < n; i++) held[i] = held[i + 1]; n--; break; }
            if (n == SYNTH_HELD) { for (unsigned i = 0; i + 1 < n; i++) held[i] = held[i + 1]; n--; }
            held[n++] = (uint8_t)key;
            s->held_n[l] = (uint8_t)n;
            int vi = find_layer_voice(s, l);
            int gated = vi >= 0 && s->voice[vi].gate;
            if (vi < 0) vi = (int)alloc_voice(s);
            voice_note(s, (unsigned)vi, l, key, layer_note(x, key), velocity, !(legato && gated));
        } else {
            if (!mono) {
                for (unsigned i = 0; i < SYNTH_VOICES; i++) {
                    struct synth_voice *v = &s->voice[i];
                    if (v->active && v->gate && v->layer == l && v->key == key) {
                        v->gate = 0;
                        v->stage = 2;
                    }
                }
                continue;
            }
            uint8_t *held = s->held[l];
            unsigned n = s->held_n[l];
            for (unsigned i = 0; i < n; i++)
                if (held[i] == key) { for (; i + 1 < n; i++) held[i] = held[i + 1]; n--; break; }
            s->held_n[l] = (uint8_t)n;
            int vi = find_layer_voice(s, l);
            if (vi < 0) continue;
            struct synth_voice *v = &s->voice[vi];
            if (v->key != key) continue;              /* an older held key went up */
            if (n) {                                  /* back to the previous held key */
                unsigned back = held[n - 1];
                voice_note(s, (unsigned)vi, l, back, layer_note(x, back), (unsigned)(v->vel >> 8), !legato);
            } else if (v->gate) {
                v->gate = 0;
                v->stage = 2;
            }
        }
    }
}

FM1_AUDIO_CODE void synth_all_off(struct synth *s)
{
    for (unsigned i = 0; i < SYNTH_VOICES; i++) {
        s->voice[i].gate = 0;
        s->voice[i].stage = 2;
    }
    for (unsigned l = 0; l < SYNTH_LAYERS; l++) s->held_n[l] = 0;
}

/* Triangle wavefold of a Q15 signal pushed past full scale (period 2^17). */
FM1_AUDIO_CODE static inline int32_t fold15(int32_t x)
{
    uint32_t t = (uint32_t)(x + 32768) & 0x1FFFFu;
    return t < 65536u ? (int32_t)t - 32768 : 98304 - (int32_t)t;
}

/* Pitch with vibrato: inc x (1 + depth x lfo). */
FM1_AUDIO_CODE static inline uint32_t vibrato(struct synth_voice *v, const struct synth_derived *x)
{
    if (!x->d) return v->inc;
    v->lfo += x->lfo_inc;
    int32_t m = (sine(v->lfo) * x->d) >> 15;           /* Q15 fraction */
    return v->inc + (uint32_t)(((int64_t)v->inc * m) >> 15);
}

FM1_AUDIO_CODE static int32_t synth_engine_sample(struct synth *s, unsigned vi, struct synth_voice *v,
                                            const struct synth_derived *x)
{
    int32_t y = 0;
    switch (x->engine) {
    case SY_BASS: {
        int32_t pe = (int32_t)(((int64_t)x->a * (int32_t)(v->env1 >> 15)) >> 15);
        uint32_t inc = v->inc + (uint32_t)(((int64_t)v->inc * pe) >> 15);
        v->env1 = decay(v->env1, x->coef1);
        y = sine(v->phase[0]);
        v->phase[0] += inc;
        if (x->gain[1]) {                              /* sub: an octave down */
            y += (sine(v->phase[1]) * x->gain[1]) >> 15;
            v->phase[1] += inc >> 1;
        }
        if (x->gain[0] && v->env2 > ENV_END) {         /* click */
            y += (int32_t)((((int64_t)white(&v->noise) * (int32_t)(v->env2 >> 15)) >> 15) * x->gain[0] >> 15);
            v->env2 = decay(v->env2, x->coef2);
        }
        if (x->b > 256) y = soft15((y * x->b) >> 8);  /* stage 1: soft clip */
        if (x->c) y = fold15((y * x->c) >> 8);         /* stage 2: fold */
        v->s0 += ((y - v->s0) * x->d) >> 15;           /* stage 3: tone low-pass */
        return v->s0;
    }
    case SY_LEAD: {
        uint32_t inc = vibrato(v, x);
        uint32_t sus = (uint32_t)x->b;
        v->env1 = v->env1 > sus ? sus + decay(v->env1 - sus, x->coef1) : sus;
        int32_t m = sine(v->phase[1] + (uint32_t)((int64_t)v->s0 * x->c));
        v->s0 = m;
        v->phase[1] += (uint32_t)(((uint64_t)inc * x->ratio_q8[0]) >> 16);
        int32_t me = (int32_t)(((int64_t)m * (int32_t)(v->env1 >> 15)) >> 15);
        y = sine(v->phase[0] + (uint32_t)((int64_t)me * x->a));
        v->phase[0] += inc;
        return y;
    }
    case SY_KOTO: {
        int16_t *buf = s->ks[vi];
        uint32_t rp = (v->pos << 16) - v->period_q16;
        uint32_t i0 = (rp >> 16) & (SYNTH_KS_LEN - 1), i1 = (i0 + 1) & (SYNTH_KS_LEN - 1);
        int32_t a = buf[i0], b = buf[i1];
        int32_t in = a + (((b - a) * (int32_t)((rp & 0xFFFFu) >> 1)) >> 15);
        v->s0 += ((in - v->s0) * x->b) >> 15;          /* damping */
        y = (v->s0 * x->c) >> 15;                      /* loop gain */
        buf[v->pos & (SYNTH_KS_LEN - 1)] = (int16_t)clamp15(y);
        v->pos++;
        if (x->gain[0]) {                              /* body: a wooden band-pass */
            int32_t body = svf2x(&v->s1, &v->s2, y, table_svf[44], 9000, 3);
            y += (body * x->gain[0]) >> 15;
        }
        return y;
    }
    case SY_FLUTE: {
        uint32_t inc = vibrato(v, x);
        uint32_t ph = v->phase[0];
        y = sine(ph);
        if (x->b) y += (sine(ph * 2u) * x->b >> 16) + (sine(ph * 3u) * x->b >> 17);
        v->phase[0] += inc;
        int32_t w = white(&v->noise);
        if (x->a) {                                    /* breath, tuned to the note */
            int32_t band = svf2x(&v->low, &v->band, w >> 3, v->s2, 2000, 3);
            y += (band * x->a) >> 15;
        }
        if (x->c && v->env2 > ENV_END) {              /* chiff at the start */
            y += (int32_t)((((int64_t)(w >> 1) * (int32_t)(v->env2 >> 15)) >> 15) * x->c >> 15);
            v->env2 = decay(v->env2, x->coef2);
        }
        if (x->gain[0]) {                              /* air: soft noise */
            v->s0 += (w - v->s0) >> 3;
            y += (v->s0 * x->gain[0]) >> 15;
        }
        return (y * 3) >> 2;
    }
    case SY_VIBES: {
        int32_t in = 0;
        if (v->s1) {                                   /* mallet: half a sine */
            in = sine(v->phase[3]);
            uint32_t next = v->phase[3] + (uint32_t)x->b;
            if (next >= 0x80000000u || next < v->phase[3]) v->s1 = 0;
            v->phase[3] = next;
        }
        y = resonate(v, 0, in) + resonate(v, 1, in) + resonate(v, 2, in);
        if (x->a) {                                    /* motor tremolo */
            v->lfo += x->lfo_inc;
            int32_t t = 32768 - ((x->a * (sine(v->lfo) + 32768)) >> 16);
            y = (int32_t)(((int64_t)y * t) >> 15);
        }
        return clamp15(y);
    }
    case SY_PAD: {
        int32_t w = white(&v->noise);
        y = resonate(v, 0, w) + resonate(v, 1, w) + resonate(v, 2, w) + resonate(v, 3, w);
        if (x->b) { v->s0 += (w - v->s0) >> 2; y += ((w - v->s0) * x->b) >> 16; }   /* air */
        return clamp15((y * 3) >> 2);
    }
    default:
        return 0;
    }
}

FM1_AUDIO_CODE static void synth_voice_render(struct synth *s, unsigned vi, int32_t *out, unsigned frames)
{
    struct synth_voice *v = &s->voice[vi];
    const struct synth_derived *x = &s->derived[v->layer < SYNTH_LAYERS ? v->layer : 0];
    if (x->engine == SY_OFF) { v->active = 0; return; }
    const int32_t gain = (int32_t)(((int64_t)x->level * v->vel) >> 15);
    for (unsigned f = 0; f < frames; f++) {
        /* glide */
        if (v->inc != v->target) {
            if (x->glide_k >= 65536u) v->inc = v->target;
            else {
                int64_t d = (int64_t)v->target - (int64_t)v->inc;
                int64_t step = (d * x->glide_k) >> 16;
                if (step == 0) step = d > 0 ? 1 : -1;
                v->inc = (uint32_t)((int64_t)v->inc + step);
            }
        }
        int32_t y = synth_engine_sample(s, vi, v, x);
        if (x->drive > 256) y = soft15((y * x->drive) >> 8);
        if (x->ftype) {
            int32_t idx = x->cutoff + (int32_t)((x->fenv * (int32_t)(v->fenv >> 15)) >> 15);
            idx = idx < 0 ? 0 : idx > 127 ? 127 : idx;
            y = svf2x(&v->low, &v->band, y, table_svf[idx], x->fq, x->ftype);
            v->fenv = decay(v->fenv, x->fenv_coef);
        }
        /* amp ADSR */
        switch (v->stage) {
        case 0:
            v->env += x->attack_inc;
            if (v->env >= Q30) { v->env = Q30; v->stage = 1; }
            break;
        case 1:
            v->env = v->env > x->sustain ? x->sustain + decay(v->env - x->sustain, x->decay_coef)
                                         : x->sustain;
            if (v->env < ENV_END && !x->sustain) { v->active = 0; return; }
            break;
        default:
            v->env = decay(v->env, x->release_coef);
            if (v->env < ENV_END) { v->active = 0; return; }
            break;
        }
        y = clamp15(y);
        y = (int32_t)(((int64_t)y * (int32_t)(v->env >> 15)) >> 15);
        y = (y * gain) >> 15;
        out[2 * f] += ((y * x->pan_l) >> 15) * 256;          /* Q15 -> Q23 */
        out[2 * f + 1] += ((y * x->pan_r) >> 15) * 256;
    }
}

FM1_AUDIO_CODE void synth_render(struct synth *s, int32_t *out, unsigned frames)
{
    for (unsigned i = 0; i < SYNTH_VOICES; i++)
        if (s->voice[i].active)
            synth_voice_render(s, i, out, frames);
}
