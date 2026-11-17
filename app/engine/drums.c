/* SPDX-License-Identifier: MIT */
#include "drums.h"

#include <string.h>

#include "fm1.h"
#include "tables.h"

#define Q30 (1u << 30)
#define CHOKE_FADE 64u

static const char *const engine_names[DRUM_ENGINES] = { "OFF", "SAMPLE", "FM" };
static const char *const param_names[DP_COUNT] = {
    "LEVEL", "PAN", "TUNE", "DECAY", "P.ENV", "P.DEC", "RATIO", "MOD",
    "M.DEC", "FDBK", "NOISE", "N.DEC", "N.TONE",
};

const char *drum_engine_name(unsigned e) { return e < DRUM_ENGINES ? engine_names[e] : "?"; }
const char *drum_param_name(unsigned p) { return p < DP_COUNT ? param_names[p] : "?"; }

/* --- kit ----------------------------------------------------------------- */

static void fm(struct drum_voice_params *v, uint8_t tune, uint8_t decay,
               uint8_t penv, uint8_t pdec, uint8_t ratio, uint8_t mod,
               uint8_t mdec, uint8_t noise, uint8_t ndec, uint8_t ntone)
{
    v->engine = DRUM_FM;
    v->p[DP_LEVEL] = 100; v->p[DP_PAN] = 64;
    v->p[DP_TUNE] = tune; v->p[DP_DECAY] = decay;
    v->p[DP_PITCH_ENV] = penv; v->p[DP_PITCH_DECAY] = pdec;
    v->p[DP_RATIO] = ratio; v->p[DP_MOD] = mod; v->p[DP_MOD_DECAY] = mdec;
    v->p[DP_FEEDBACK] = 0;
    v->p[DP_NOISE] = noise; v->p[DP_NOISE_DECAY] = ndec; v->p[DP_NOISE_TONE] = ntone;
}

void drum_voice_fm_kick(struct drum_voice_params *v)
{
    uint8_t choke = v->choke;
    fm(v, 30, 70, 70, 25, 1, 10, 20, 0, 10, 40);
    v->choke = choke;
}

void drum_kit_default(struct drum_kit *k)
{
    memset(k, 0, sizeof *k);
    /*      voice            tune dec  penv pdec rat mod mdec noise ndec ntone */
    fm(&k->voice[0],         30,  70,  70,  25,  1,  10,  20,   0,   10,  40);  /* FM kick */
    k->voice[0].engine = DRUM_SAMPLE;                                        /* kick sample */
    k->voice[0].p[DP_TUNE] = 64;    /* original speed */
    k->voice[0].p[DP_DECAY] = 127;  /* let the whole sample ring */
    fm(&k->voice[1],         62,  45,  30,  15,  3,  30,  20,  90,   50,  95);  /* snare */
    fm(&k->voice[2],        110,  20,   0,   0,  9,  90,  20,  70,   18, 120);  /* closed hat */
    fm(&k->voice[3],        110,  58,   0,   0,  9,  90,  45,  70,   55, 120);  /* open hat */
    fm(&k->voice[4],         42,  60,  35,  30,  1,   8,  20,   5,   15,  50);  /* low tom */
    fm(&k->voice[5],         55,  55,  35,  30,  1,   8,  20,   5,   15,  50);  /* high tom */
    fm(&k->voice[6],         70,  30,   0,   0,  1,   0,   0, 110,   35,  85);  /* clap-ish noise */
    fm(&k->voice[7],         95,  12,  10,  10,  5,  60,  10,  20,   10, 110);  /* rim */
    fm(&k->voice[8],         84,  48,   0,   0,  5,  45,  40,   0,   10,  64);  /* cowbell */
    fm(&k->voice[9],         24,  80,  90,  35,  0,  25,  30,   0,   10,  40);  /* deep FM kick */
    fm(&k->voice[10],        96,  75,   0,   0, 14, 110,  70,  40,   70, 100);  /* metal crash */
    k->voice[2].choke = 1;   /* closed and open hat choke each other */
    k->voice[3].choke = 1;
}

/* --- prepare (control task) --------------------------------------------- */

static int32_t clamp127(int v) { return v < 0 ? 0 : v > 127 ? 127 : v; }

void drums_prepare_voice(struct drums *d, const struct drum_kit *kit, unsigned v)
{
    const struct drum_voice_params *p = &kit->voice[v];
    struct drum_derived x;
    memset(&x, 0, sizeof x);
    x.engine = p->engine < DRUM_ENGINES ? p->engine : DRUM_OFF;
    x.choke = p->choke <= DRUM_CHOKE_GROUPS ? p->choke : 0;
    x.base_inc = table_tune[clamp127(p->p[DP_TUNE])];
    /* Pitch sweep: up to 16x the base frequency at the envelope's peak. */
    uint64_t sweep = (uint64_t)x.base_inc * (uint64_t)p->p[DP_PITCH_ENV] * 15u / 127u;
    x.sweep = sweep > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)sweep;
    x.amp_coef = table_decay[clamp127(p->p[DP_DECAY])];
    x.pitch_coef = table_decay[clamp127(p->p[DP_PITCH_DECAY])];
    x.mod_coef = table_decay[clamp127(p->p[DP_MOD_DECAY])];
    x.noise_coef = table_decay[clamp127(p->p[DP_NOISE_DECAY])];
    x.ratio_q8 = table_ratio[p->p[DP_RATIO] & 15u];
    /* Modulation index up to ~8 rad: phase offset per unit of Q15 sine. */
    x.mod_depth = (int32_t)p->p[DP_MOD] * 1314;
    x.feedback = (int32_t)p->p[DP_FEEDBACK] * 160;
    unsigned tone = (unsigned)clamp127(p->p[DP_NOISE_TONE]);
    x.noise_highpass = tone >= 64;
    unsigned t = x.noise_highpass ? tone - 64 : tone;
    x.noise_a = (int32_t)(t + 1) * 512;                 /* 512 .. 32768 */
    if (x.noise_highpass) x.noise_a = 32768 - x.noise_a + 512; /* low cut moves up */
    x.noise_level = (int32_t)p->p[DP_NOISE] * 258;      /* 0 .. ~32766 */
    x.level = (int32_t)p->p[DP_LEVEL] * 258;
    unsigned pan = (unsigned)clamp127(p->p[DP_PAN]);
    x.pan_l = pan <= 64 ? 32767 : (int32_t)((127 - pan) * 32767 / 63);
    x.pan_r = pan >= 64 ? 32767 : (int32_t)(pan * 32767 / 64);
    /* Sample tune: 64 = original speed, +-64 = +-1 octave (exponential). */
    int st = (int)clamp127(p->p[DP_TUNE]) - 64;
    uint32_t step = 65536;
    /* 2^(st/64) from the tune table: entries are 20 Hz * 100^(i/127), so a
     * ratio of one octave is 127/log2(100) ~= 19.1 entries. */
    int idx = 64 + st * 191 / 640;
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    step = (uint32_t)(((uint64_t)table_tune[idx] << 16) / table_tune[64]);
    x.sample_step = step;
    d->derived[v] = x;
}

void drums_prepare(struct drums *d, const struct drum_kit *kit)
{
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        drums_prepare_voice(d, kit, v);
}

void drums_init(struct drums *d)
{
    memset(d, 0, sizeof *d);
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        d->voice[v].noise = 0x9E3779B9u ^ (v * 0x85EBCA6Bu);
}

/* --- audio interrupt ----------------------------------------------------- */

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
    /* Field by field: a struct clear could become a memset call, and libc
     * lives in flash, which the audio interrupt must not touch. */
    if (!s->noise) s->noise = 1;
    s->fade = 0;
    s->car_phase = s->mod_phase = 0;
    s->mod_prev = 0;
    s->pos_q16 = 0;
    s->lp = 0;
    s->amp_env = s->pitch_env = s->mod_env = s->noise_env = Q30;
    s->active = 1;
    if (velocity > 127) velocity = 127;
    /* level x velocity / 127, without a division (none in audio code). */
    s->gain = (int32_t)(((int64_t)x->level * (int32_t)velocity * 516) >> 16);
}

FM1_AUDIO_CODE static void render_voice(struct drums *d, unsigned v, int32_t *out,
                                        unsigned from, unsigned to)
{
    struct drum_voice_state *s = &d->voice[v];
    const struct drum_derived *x = &d->derived[v];
    for (unsigned f = from; f < to && s->active; f++) {
        int32_t y;
        if (x->engine == DRUM_SAMPLE) {
            uint32_t i = s->pos_q16 >> 16;
            if (!d->sample.pcm || i + 1 >= d->sample.frames) { s->active = 0; break; }
            int32_t a = d->sample.pcm[i], b = d->sample.pcm[i + 1];
            y = a + (((b - a) * (int32_t)((s->pos_q16 & 0xFFFFu) >> 1)) >> 15);
            s->pos_q16 += x->sample_step;
            y = (int32_t)(((int64_t)y * (int32_t)(s->amp_env >> 15)) >> 15);
            s->amp_env = decay(s->amp_env, x->amp_coef);
        } else {
            /* Modulator with feedback, then carrier with pitch sweep. */
            uint32_t inc = x->base_inc +
                (uint32_t)(((uint64_t)x->sweep * (s->pitch_env >> 14)) >> 16);
            uint32_t minc = (uint32_t)(((uint64_t)inc * x->ratio_q8) >> 8);
            int32_t m = sine(s->mod_phase + (uint32_t)(s->mod_prev * x->feedback));
            s->mod_prev = m;
            s->mod_phase += minc;
            int32_t menv = (int32_t)(s->mod_env >> 15);           /* Q15 */
            /* Up to ~1.3 cycles; phase arithmetic wraps, so truncation is fine. */
            uint32_t offset = (uint32_t)(((int64_t)m * x->mod_depth * menv) >> 15);
            int32_t c = sine(s->car_phase + offset);
            s->car_phase += inc;
            int32_t aenv = (int32_t)(s->amp_env >> 15);
            y = (c * aenv) >> 15;

            /* Noise: xorshift white, one-pole low- or high-pass. */
            uint32_t n = s->noise;
            n ^= n << 13; n ^= n >> 17; n ^= n << 5;
            s->noise = n;
            int32_t w = (int32_t)(int16_t)(n >> 16);
            s->lp += ((w - s->lp) * x->noise_a) >> 15;
            int32_t filtered = x->noise_highpass ? w - s->lp : s->lp;
            int32_t nenv = (int32_t)(s->noise_env >> 15);
            y += (int32_t)(((int64_t)filtered * nenv >> 15) * x->noise_level >> 15);

            s->amp_env = decay(s->amp_env, x->amp_coef);
            s->pitch_env = decay(s->pitch_env, x->pitch_coef);
            s->mod_env = decay(s->mod_env, x->mod_coef);
            s->noise_env = decay(s->noise_env, x->noise_coef);
            if (s->amp_env < 64 && s->noise_env < 64) { s->active = 0; }
        }
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        y = (y * s->gain) >> 15;
        if (s->fade) {
            y = (y * (int32_t)s->fade) >> 6; /* / CHOKE_FADE (64) */
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
