/* SPDX-License-Identifier: MIT */
#include "engine/engine.h"

#include <string.h>

#include "engine/mtables.h"
#include "fm1.h"

#define Q30 (1u << 30)
#define SILENT 6                 /* peak below this for QUIET_BLOCKS: voice ends */
#define QUIET_BLOCKS 24
#define KS_MASK (MV_KS_LEN - 1u)

/* --- names ----------------------------------------------------------------- */

static const char *const engine_names[MV_ENGINES] = { "KOTO" };
static const char *const koto_names[KT_COUNT] = {
    "DECAY", "BRITE", "STIFF", "DAMP",
    "POS", "NAIL", "FORCE", "NOISE",
    "BODY", "SIZE", "SAWRI", "TENSN",
    "OSHID", "PRESS", "B.IN", "B.TIM",
    "ATO", "A.DLY", "YURI", "Y.RAT",
    "HUMAN", "WIDTH", "LEVEL",
};
static const char *const tuning_names[TU_COUNT] = { "EQUAL", "JUST", "PYTH" };
static const char *const scale_names[SC_COUNT] = {
    "CHROM", "MAJOR", "MINOR", "HIRA", "KUMOI", "KOKIN", "IWATO", "AKEBO", "YO",
};
static const char *const map_names[MAP_COUNT] = { "KEYS", "WHITE" };

const char *mv_engine_name(unsigned e) { return e < MV_ENGINES ? engine_names[e] : "?"; }
const char *mv_param_name(unsigned e, unsigned i)
{
    if (e == MV_KOTO && i < KT_COUNT) return koto_names[i];
    return 0;
}
const char *mv_tuning_name(unsigned t) { return t < TU_COUNT ? tuning_names[t] : "?"; }
const char *mv_scale_name(unsigned s) { return s < SC_COUNT ? scale_names[s] : "?"; }
const char *mv_map_name(unsigned m) { return m < MAP_COUNT ? map_names[m] : "?"; }

/* --- patches and keyboard (control task) ---------------------------------- */

void mv_patch_default(struct mv_patch *p, unsigned engine)
{
    static const uint8_t koto[KT_COUNT] = {
        [KT_DECAY] = 70, [KT_BRIGHT] = 80, [KT_STIFF] = 20, [KT_DAMP] = 40,
        [KT_POS] = 25, [KT_NAIL] = 90, [KT_STRENGTH] = 80, [KT_NOISE] = 40,
        [KT_BODY] = 60, [KT_SIZE] = 64, [KT_SAWARI] = 15, [KT_TENSION] = 50,
        [KT_OSHIDE] = 64, [KT_PRESS] = 50, [KT_BENDIN] = 0, [KT_BENDTIME] = 40,
        [KT_ATO] = 0, [KT_ATODELAY] = 60, [KT_YURI] = 0, [KT_YURIRATE] = 50,
        [KT_HUMAN] = 25, [KT_WIDTH] = 70, [KT_LEVEL] = 100,
    };
    memset(p, 0, sizeof *p);
    p->engine = (uint8_t)(engine < MV_ENGINES ? engine : MV_KOTO);
    memcpy(p->p, koto, sizeof koto);
}

void mv_global_default(struct mv_global *g)
{
    memset(g, 0, sizeof *g);
    g->tuning = TU_EQUAL;
    g->scale = SC_HIRAJOSHI;
    g->map = MAP_KEYS;
    g->root = 2;                 /* D */
    g->voices = 6;
}

static const uint8_t scale_steps[SC_COUNT][13] = {
    /* count, then degrees */
    { 12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },
    { 7, 0, 2, 4, 5, 7, 9, 11 },
    { 7, 0, 2, 3, 5, 7, 8, 10 },
    { 5, 0, 2, 3, 7, 8 },        /* hirajoshi */
    { 5, 0, 1, 5, 7, 8 },        /* kumoi */
    { 5, 0, 1, 5, 7, 10 },       /* kokin */
    { 5, 0, 1, 5, 6, 10 },       /* iwato */
    { 5, 0, 2, 3, 7, 9 },        /* akebono */
    { 5, 0, 2, 5, 7, 9 },        /* yo */
};
static const uint8_t white_keys[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };

int mv_key_note(const struct mv_global *g, unsigned key, int octave)
{
    int n;
    if (key > 26) return -1;
    if (g->map == MAP_WHITE) {
        unsigned wi = 0, black = 1;
        for (unsigned i = 0; i < 16; i++) {
            if (white_keys[i] == key) { wi = i; black = 0; break; }
            if (white_keys[i] < key) wi = i;
        }
        const uint8_t *s = scale_steps[g->scale < SC_COUNT ? g->scale : 0];
        unsigned count = s[0];
        n = 48 + (g->root % 12) + (int)(wi / count) * 12 + s[1 + wi % count] + (int)black;
    } else {
        n = 53 + (int)key;
    }
    n += octave * 12;
    return n < 0 || n > 127 ? -1 : n;
}

/* --- prepare (control task) ------------------------------------------------ */

static unsigned c127(unsigned v) { return v > 127 ? 127 : v; }

/* lo x ratio^v, ratio in Q16 (exponential parameter maps). */
static uint32_t expmap(uint32_t lo, uint32_t ratio_q16, unsigned v)
{
    uint64_t r = (uint64_t)lo << 8;
    for (unsigned i = 0; i < v; i++) r = (r * ratio_q16) >> 16;
    return (uint32_t)(r >> 8);
}

/* log2 attenuation per sample (Q32) for a 60 dB decay over t_ms. */
static uint32_t atten_for_ms(uint32_t t_ms)
{
    if (!t_ms) t_ms = 1;
    return (uint32_t)((9965784ULL << 32) / ((uint64_t)t_ms * 44100000ULL));
}

/* Per-block glide factor (Q16) for a time constant of t_ms. */
static int32_t glide_k(uint32_t t_ms)
{
    uint64_t k = 65536ULL * 12800u / ((uint64_t)(t_ms ? t_ms : 1) * 441u);
    return k > 65536 ? 65536 : (int32_t)k;
}

static int32_t sin_q15(uint32_t phase)
{
    uint32_t i = phase >> 22, frac = (phase >> 6) & 0xFFFFu;
    int32_t a = mt_sine[i], b = mt_sine[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)frac) >> 16);
}

static void koto_prepare(struct koto_derived *k, const uint8_t *p)
{
    memset(k, 0, sizeof *k);
    k->atten_q32 = atten_for_ms(expmap(300, 67954, c127(p[KT_DECAY])));      /* 0.3 .. 30 s */
    unsigned damp = c127(p[KT_DAMP]);
    k->release_q32 = damp ? atten_for_ms(expmap(40, 68078, 127 - damp)) : 0; /* 5 s .. 40 ms */
    k->s = 16384 - (int32_t)c127(p[KT_BRIGHT]) * 113;
    unsigned stiff = c127(p[KT_STIFF]);
    k->ap = -(int32_t)stiff * 180;
    k->comp_q16 = (uint32_t)k->s * 2u;
    if (stiff)   /* allpass phase delay at low frequencies: (1 - a) / (1 + a) */
        k->comp_q16 += (uint32_t)(((uint64_t)(32768 - k->ap) << 16) / (uint32_t)(32768 + k->ap));
    k->pos = 1300 + (int32_t)c127(p[KT_POS]) * 120;
    if (k->pos > 16384) k->pos = 16384;
    unsigned nail = c127(p[KT_NAIL]), force = c127(p[KT_STRENGTH]);
    k->exc_len = 8u + (127u - nail) * 82u / 127u;
    k->exc_inc = 0x80000000u / k->exc_len;
    k->exc_lp = 3000 + (int32_t)nail * 200 + (int32_t)force * 20;
    if (k->exc_lp > 32767) k->exc_lp = 32767;
    k->exc_amp = 6000 + (int32_t)force * 180;
    k->exc_noise = (int32_t)c127(p[KT_NOISE]) * 258;
    unsigned saw = c127(p[KT_SAWARI]);
    k->saw_th = saw ? 26000 - (int32_t)saw * 160 : 0x7FFFFFFF;
    k->saw_r = 32767 - (int32_t)saw * 200;
    k->tension_q16 = (int32_t)c127(p[KT_TENSION]) * 200;
    k->oshide_q16 = (int32_t)c127(p[KT_OSHIDE]) * 1032;
    k->press_k = glide_k(expmap(10, 67954, c127(p[KT_PRESS])));             /* 10 ms .. 1 s */
    k->bendin_q16 = (int32_t)c127(p[KT_BENDIN]) * 1032;
    k->bendin_k = glide_k(expmap(10, 67954, c127(p[KT_BENDTIME])));
    k->ato_q16 = (int32_t)c127(p[KT_ATO]) * 1032;
    k->ato_delay = c127(p[KT_ATODELAY]) * 4u;                                 /* 0 .. 1.5 s */
    k->yuri_q16 = (int32_t)c127(p[KT_YURI]) * 258;
    k->yuri_inc = 6233000u + c127(p[KT_YURIRATE]) * 735000u;                 /* 0.5 .. 8 Hz */
    k->human_cents_q16 = (int32_t)c127(p[KT_HUMAN]) * 77;                    /* up to +-15 cents */
    k->human_gain = (int32_t)c127(p[KT_HUMAN]) * 40;
    k->width = (int32_t)c127(p[KT_WIDTH]) * 258;
    k->level = (int32_t)c127(p[KT_LEVEL]) * 258;

    /* Body: three wooden modes, scaled by SIZE, Q ~ 12. */
    static const uint32_t body_hz[3] = { 185, 320, 510 };
    uint32_t scale_q16 = 39322u + c127(p[KT_SIZE]) * 516u;                     /* 0.6 .. 1.6 */
    for (unsigned i = 0; i < 3; i++) {
        uint32_t f = (uint32_t)(((uint64_t)body_hz[i] * scale_q16) >> 16);
        uint32_t inc = f * 97392u;
        uint64_t omr = ((uint64_t)inc * 4289u) >> 16;                         /* (1 - r), Q30 */
        uint32_t r = Q30 - (uint32_t)omr;
        int64_t sh = sin_q15(inc >> 1);
        int64_t cw = (int64_t)Q30 - 2 * sh * sh;
        k->body_b1[i] = (int32_t)((2 * (int64_t)r * cw) >> 30);
        k->body_b2[i] = (int32_t)(((int64_t)r * r) >> 30);
        k->body_g[i] = (int32_t)(((int64_t)omr * sin_q15(inc) * 2) >> 15);    /* Q30 */
    }
    k->body_mix = (int32_t)c127(p[KT_BODY]) * 258;
}

void mv_prepare(struct mavis_engine *e, const struct mv_patch *patch, const struct mv_global *g)
{
    /* Cents from equal temperament for each pitch class above the root. */
    static const int16_t cents_x100[TU_COUNT][12] = {
        { 0 },
        { 0, 1173, 391, 1564, -1369, -196, -978, 196, 1369, -1564, 1760, -1173 },
        { 0, -978, 391, -587, 782, -196, 1173, 196, -782, 587, -391, 978 },
    };
    unsigned t = g->tuning < TU_COUNT ? g->tuning : 0;
    for (unsigned i = 0; i < 12; i++)
        e->tuning_q16[i] = (int32_t)cents_x100[t][i] * 65536 / 10000;
    e->global = *g;
    if (e->global.voices < 1) e->global.voices = 1;
    if (e->global.voices > MV_MAX_VOICES) e->global.voices = MV_MAX_VOICES;
    koto_prepare(&e->koto, patch->p);
}

void mv_init(struct mavis_engine *e)
{
    memset(e, 0, sizeof *e);
    e->random = 0x6D617673u;    /* "mavs" */
    for (unsigned i = 0; i < MV_MAX_VOICES; i++)
        e->voice[i].noise = 0x9E3779B9u * (i + 1u);
}

/* --- audio interrupt ------------------------------------------------------- */

FM1_AUDIO_CODE static inline int32_t sine1024(uint32_t phase)
{
    uint32_t i = phase >> 22, frac = (phase >> 6) & 0xFFFFu;
    int32_t a = mt_sine[i], b = mt_sine[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)frac) >> 16);
}

FM1_AUDIO_CODE static inline uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

FM1_AUDIO_CODE static inline int32_t clamp16(int32_t y)
{
    return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

/* Pitch (semitones Q16) to period (samples Q16). */
FM1_AUDIO_CODE static uint32_t pitch_period(int32_t pitch_q16)
{
    if (pitch_q16 < 0) pitch_q16 = 0;
    if (pitch_q16 > (127 << 16)) pitch_q16 = 127 << 16;
    uint32_t n = (uint32_t)pitch_q16 >> 16, f = ((uint32_t)pitch_q16 >> 8) & 255u;
    return (uint32_t)(((uint64_t)mt_note_period[n] * mt_semi_down[f]) >> 16);
}

FM1_AUDIO_CODE static void voice_start(struct mavis_engine *e, unsigned vi, unsigned note, int fresh)
{
    struct mv_voice *v = &e->voice[vi];
    const struct koto_derived *k = &e->koto;
    if (fresh) {
        volatile int16_t *line = e->line[vi];      /* volatile: no memset call */
        for (unsigned i = 0; i < MV_KS_LEN; i++) line[i] = 0;
        v->w = 0;
        v->x1 = v->ap_x1 = v->ap_y1 = 0;
        v->press_q16 = v->ato_q16 = 0;
        v->lfo = 0;
        v->peak = 0;
    }
    v->note = (uint8_t)note;
    v->gate = 1;
    v->pending = 0;
    v->quiet = 0;
    v->blocks = 0;
    v->age = e->clock++;
    v->exc_i = 0;
    v->exc_phase = 0;
    v->exc_lpf = 0;
    v->tension_env = 65536;
    v->bendin_q16 = -k->bendin_q16;

    /* Tuning from the root, then humanize (stands in for touch). */
    int c = (int)note + 120 - (int)e->global.root;
    while (c >= 12) c -= 12;
    uint32_t r = xorshift(&e->random);
    int32_t hum = (int32_t)(((int64_t)((int32_t)(r & 0xFFFF) - 32768) * k->human_cents_q16) >> 15);
    v->pitch_q16 = ((int32_t)note << 16) + e->tuning_q16[c] + hum;
    int32_t hg = (int32_t)(((int64_t)((int32_t)(r >> 16) - 32768) * k->human_gain) >> 15);
    v->gain = (int32_t)(((int64_t)k->level * (32768 + hg)) >> 15);

    /* Spread across the stereo field by pitch, like strings across a koto. */
    int32_t pos = ((int32_t)note - 36) * 455;                 /* 0 at C2 .. 32760 at C8 */
    pos = pos < 0 ? 0 : pos > 32767 ? 32767 : pos;
    int32_t pan = 16384 + (int32_t)(((int64_t)(pos - 16384) * k->width) >> 15);
    v->pan_r = pan;
    v->pan_l = 32767 - pan;
    v->active = 1;
    e->last_voice = (uint8_t)vi;
}

FM1_AUDIO_CODE void mv_note(struct mavis_engine *e, unsigned note, unsigned velocity)
{
    const unsigned n = e->global.voices;
    if (note > 127) return;
    if (!velocity) {
        for (unsigned i = 0; i < n; i++)
            if (e->voice[i].active && e->voice[i].gate && e->voice[i].note == note)
                e->voice[i].gate = 0;
        return;
    }
    /* The same string again: re-pluck it (keeps ringing). */
    for (unsigned i = 0; i < n; i++)
        if (e->voice[i].active && !e->voice[i].pending && e->voice[i].note == note) {
            voice_start(e, i, note, 0);
            return;
        }
    for (unsigned i = 0; i < n; i++)
        if (!e->voice[i].active) { voice_start(e, i, note, 1); return; }
    /* Steal: the quietest released voice, else the oldest; fade it out over
     * one block and start the new note after. */
    unsigned best = n;
    int32_t best_peak = 0x7FFFFFFF;
    for (unsigned i = 0; i < n; i++)
        if (!e->voice[i].gate && !e->voice[i].pending && e->voice[i].peak < best_peak) {
            best_peak = e->voice[i].peak; best = i;
        }
    if (best == n) {
        uint32_t oldest = 0;
        for (unsigned i = 0; i < n; i++)
            if (!e->voice[i].pending && e->clock - e->voice[i].age >= oldest) {
                oldest = e->clock - e->voice[i].age; best = i;
            }
    }
    if (best == n) return;
    e->voice[best].pending = 1;
    e->voice[best].pending_note = (uint8_t)note;
}

FM1_AUDIO_CODE void mv_all_off(struct mavis_engine *e)
{
    for (unsigned i = 0; i < MV_MAX_VOICES; i++) e->voice[i].gate = 0;
    e->pressing = 0;
}

/* Per-block control of one string: bends, period, taps, loop gain. */
FM1_AUDIO_CODE static void koto_block(struct mavis_engine *e, struct mv_voice *v)
{
    const struct koto_derived *k = &e->koto;
    v->bendin_q16 -= (int32_t)(((int64_t)v->bendin_q16 * k->bendin_k) >> 16);
    int32_t press_t = e->pressing && v->gate ? k->oshide_q16 : 0;
    v->press_q16 += (int32_t)(((int64_t)(press_t - v->press_q16) * k->press_k) >> 16);
    int late = v->blocks >= k->ato_delay;
    int32_t ato_t = late && v->gate ? k->ato_q16 : 0;
    v->ato_q16 += (int32_t)(((int64_t)(ato_t - v->ato_q16) * k->press_k) >> 16);
    int32_t vib = 0;
    if (k->yuri_q16 && late) {
        v->lfo += k->yuri_inc;
        vib = (int32_t)(((int64_t)sine1024(v->lfo) * k->yuri_q16) >> 15);
    }
    int32_t tension = (int32_t)(((int64_t)k->tension_q16 * v->tension_env) >> 16);
    v->tension_env = (int32_t)(((int64_t)v->tension_env * 61000) >> 16);   /* ~40 ms */
    int32_t pitch = v->pitch_q16 + v->bendin_q16 + v->press_q16 + v->ato_q16 + vib + tension;

    uint32_t period = pitch_period(pitch);
    uint32_t d = period > k->comp_q16 ? period - k->comp_q16 : 0;
    if (d < (3u << 16)) d = 3u << 16;
    if (d > ((MV_KS_LEN - 4u) << 16)) d = (MV_KS_LEN - 4u) << 16;
    v->pint = d >> 16;
    /* 4-point Lagrange at t = 1 + frac over taps at delays pint-1 .. pint+2. */
    int64_t t = 65536 + (int64_t)(d & 0xFFFFu);
    int64_t t1 = t - 65536, t2 = t - 131072, t3 = t - 196608;
    int64_t a01 = (t * t1) >> 16, a23 = (t2 * t3) >> 16;
    v->c0 = (int32_t)(-(((t1 * a23) >> 16) * 10923 >> 16) >> 1);      /* -(t-1)(t-2)(t-3)/6 */
    v->c1 = (int32_t)((((t * a23) >> 16) >> 1) >> 1);                   /*  t(t-2)(t-3)/2 */
    v->c2 = (int32_t)(-(((a01 * t3) >> 16) >> 1) >> 1);                 /* -t(t-1)(t-3)/2 */
    v->c3 = (int32_t)((((a01 * t2) >> 16) * 10923 >> 16) >> 1);         /*  t(t-1)(t-2)/6 */

    /* Loop gain per pass from the decay time; faster after key up. */
    uint64_t x = ((uint64_t)k->atten_q32 * period) >> 32;                  /* log2 units, Q16 */
    if (!v->gate) x += ((uint64_t)k->release_q32 * period) >> 32;
    uint32_t ip = (uint32_t)(x >> 16);
    v->loop_gain = ip >= 16 ? 0 : (int32_t)(mt_exp2_neg[(x >> 8) & 255u] >> ip);
    v->comb = (uint32_t)(((uint64_t)(period >> 16) * (uint32_t)k->pos) >> 15);
    if (v->comb < 1) v->comb = 1;
}

/* Render one string into out (stereo Q23) and the mono body bus (Q15).
 * fade: ramp to silence over the block (the voice is being stolen). */
FM1_AUDIO_CODE static void koto_render(struct mavis_engine *e, unsigned vi, int32_t *out,
                                       int32_t *bus, unsigned frames, int fade)
{
    struct mv_voice *v = &e->voice[vi];
    const struct koto_derived *k = &e->koto;
    int16_t *line = e->line[vi];
    int32_t peak = 0;
    int32_t ramp = 32767, ramp_step = fade ? 32767 / 128 : 0;
    for (unsigned f = 0; f < frames; f++) {
        uint32_t r = v->w - v->pint;
        int32_t a = line[(r + 1u) & KS_MASK], b = line[r & KS_MASK];
        int32_t c = line[(r - 1u) & KS_MASK], d = line[(r - 2u) & KS_MASK];
        int32_t x = (v->c0 * a + v->c1 * b + v->c2 * c + v->c3 * d) >> 15;
        int32_t y = ((32768 - k->s) * x + k->s * v->x1) >> 15;           /* damping */
        v->x1 = x;
        y = (int32_t)(((int64_t)y * v->loop_gain) >> 16);
        if (k->ap) {                                                    /* stiffness */
            int32_t o = ((k->ap * (y - v->ap_y1)) >> 15) + v->ap_x1;
            v->ap_x1 = y;
            v->ap_y1 = o;
            y = o;
        }
        if (y > k->saw_th) y = k->saw_th + (((y - k->saw_th) * k->saw_r) >> 15);   /* sawari */
        if (v->exc_i < k->exc_len) {                                    /* the pluck */
            int32_t win = sine1024(v->exc_phase);
            v->exc_phase += k->exc_inc;
            int32_t n = (int32_t)(int16_t)(xorshift(&v->noise) >> 16);
            int32_t src = ((n * k->exc_noise) >> 15) + (((32767 - k->exc_noise) * 32767) >> 15);
            int32_t ex = (((src * win) >> 15) * k->exc_amp) >> 15;
            v->exc_lpf += ((ex - v->exc_lpf) * k->exc_lp) >> 15;
            y += v->exc_lpf;
            v->exc_i++;
        }
        y = clamp16(y);
        line[v->w & KS_MASK] = (int16_t)y;
        v->w++;
        /* Pluck/pickup position: comb against the string a little back. */
        int32_t o = (y - line[(v->w - 1u - v->comb) & KS_MASK]) >> 1;
        o = (o * v->gain) >> 15;
        if (fade) { o = (o * ramp) >> 15; ramp -= ramp_step; if (ramp < 0) ramp = 0; }
        int32_t m = o < 0 ? -o : o;
        if (m > peak) peak = m;
        bus[f] += o;
        out[2 * f] += ((o * v->pan_l) >> 15) * 256;
        out[2 * f + 1] += ((o * v->pan_r) >> 15) * 256;
    }
    v->peak = peak;
}

FM1_AUDIO_CODE void mv_render(struct mavis_engine *e, int32_t *out, unsigned frames)
{
    int32_t bus[128];
    if (frames > 128) frames = 128;
    volatile int32_t *clear = bus;
    for (unsigned f = 0; f < frames; f++) clear[f] = 0;

    for (unsigned i = 0; i < MV_MAX_VOICES; i++) {
        struct mv_voice *v = &e->voice[i];
        if (!v->active) continue;
        koto_block(e, v);
        koto_render(e, i, out, bus, frames, v->pending);
        v->blocks++;
        if (v->pending) { voice_start(e, i, v->pending_note, 1); continue; }
        if (v->peak < SILENT && v->exc_i >= e->koto.exc_len) {
            if (++v->quiet >= QUIET_BLOCKS) v->active = 0;
        } else {
            v->quiet = 0;
        }
    }

    /* Shared body: three wooden modes on the summed strings. */
    const struct koto_derived *k = &e->koto;
    if (k->body_mix) {
        for (unsigned f = 0; f < frames; f++) {
            int32_t x = bus[f];
            int64_t y = 0;
            for (unsigned m = 0; m < 3; m++) {
                int64_t r = ((int64_t)k->body_b1[m] * e->body_y1[m] -
                             (int64_t)k->body_b2[m] * e->body_y2[m]) >> 30;
                r += ((int64_t)x * k->body_g[m]) >> 22;      /* Q15 in, Q23 state */
                if (r > (1 << 30)) r = 1 << 30;
                if (r < -(1 << 30)) r = -(1 << 30);
                e->body_y2[m] = e->body_y1[m];
                e->body_y1[m] = (int32_t)r;
                y += r;
            }
            int32_t yo = (int32_t)((y * k->body_mix) >> 16);   /* Q23, half level */
            out[2 * f] += yo;
            out[2 * f + 1] += yo;
        }
    }
}
