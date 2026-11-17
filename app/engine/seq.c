/* SPDX-License-Identifier: MIT */
#include "seq.h"

#include <string.h>

#include "fm1.h" /* FM1_AUDIO_CODE / FM1_AUDIO_DATA: seq_render runs in the audio interrupt */

/* One pulse lasts SEQ_RATE * 60 / (bpm * 4 * 12) frames. With bpm in 1/100,
 * the phase gains bpm100 * 48 per frame and a pulse is due at PULSE_UNIT. */
#define PULSE_UNIT (SEQ_RATE * 60u * 100u)
#define PHASE_STEP(bpm100) ((uint32_t)(bpm100) * 4u * SEQ_PULSES_PER_STEP)

static uint8_t speed_period[SEQ_SPEED_COUNT] FM1_AUDIO_DATA = { 6, 12, 16, 24, 48 };

/* Ratchet hits that fit on whole pulses, and the pulses between them, per
 * speed and requested hits (1..4). No division in the audio interrupt. */
static uint8_t ratchet_hits[SEQ_SPEED_COUNT][4] FM1_AUDIO_DATA = {
    { 1, 2, 3, 3 }, { 1, 2, 3, 4 }, { 1, 2, 2, 4 }, { 1, 2, 3, 4 }, { 1, 2, 3, 4 },
};
static uint8_t ratchet_gap[SEQ_SPEED_COUNT][4] FM1_AUDIO_DATA = {
    { 6, 3, 2, 2 }, { 12, 6, 4, 3 }, { 16, 8, 8, 4 }, { 24, 12, 8, 6 }, { 48, 24, 16, 12 },
};

FM1_AUDIO_CODE unsigned seq_speed_period(unsigned speed)
{
    return speed < SEQ_SPEED_COUNT ? speed_period[speed] : 12u;
}

void seq_pattern_init(struct seq_pattern *p)
{
    memset(p, 0, sizeof *p);
    p->length = 16;
    p->bpm100 = 12000;
    for (unsigned v = 0; v < SEQ_VOICES; v++) {
        struct seq_voice_track *t = &p->voices[v];
        t->length = 16;
        t->speed = SEQ_SPEED_1X;
        t->swing = 50;
        for (unsigned s = 0; s < SEQ_STEPS; s++) {
            t->steps[s].velocity = 100;
            t->steps[s].probability = 100;
            t->steps[s].ratchet = 1;
        }
    }
}

void seq_start(struct seq_state *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->random = seed ? seed : 0x23F00DD5u;
    s->phase = PULSE_UNIT; /* the first pulse is due at frame 0 */
    s->playing = 1;
}

void seq_stop(struct seq_state *s)
{
    s->playing = 0;
    memset(s->pending_left, 0, sizeof s->pending_left);
}

FM1_AUDIO_CODE static uint32_t next_random(struct seq_state *s)
{
    uint32_t x = s->random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s->random = x;
}

FM1_AUDIO_CODE unsigned seq_voice_step(const struct seq_state *s, const struct seq_pattern *p,
                        unsigned voice)
{
    (void)p;
    return s->vindex[voice];
}

/* Everything due at the current pulse, emitted at frame. */
FM1_AUDIO_CODE static unsigned on_pulse(struct seq_state *s, const struct seq_pattern *p,
                         uint16_t frame, struct seq_event *ev, unsigned n,
                         unsigned max)
{
    for (unsigned v = 0; v < SEQ_VOICES; v++) {
        const struct seq_voice_track *t = &p->voices[v];

        /* Hits waiting from an earlier pulse of this step. */
        if (s->pending_left[v]) {
            if (s->pending_wait[v] == 0) {
                if (n < max && !t->mute)
                    ev[n++] = (struct seq_event){ frame, (uint8_t)v, s->pending_velocity[v] };
                if (--s->pending_left[v])
                    s->pending_wait[v] = (uint8_t)(s->pending_gap[v] - 1);
            } else {
                s->pending_wait[v]--;
            }
        }

        unsigned speed = t->speed < SEQ_SPEED_COUNT ? t->speed : SEQ_SPEED_1X;
        unsigned period = speed_period[speed];
        unsigned len = t->length < 1 ? 1 : t->length > SEQ_STEPS ? SEQ_STEPS : t->length;
        /* Length or speed may change while playing. */
        if (s->vindex[v] >= len) s->vindex[v] = 0;
        if (s->vpulse[v] >= period) s->vpulse[v] = 0;
        unsigned at = s->vpulse[v];
        unsigned index = s->vindex[v];
        if (++s->vpulse[v] >= period) {
            s->vpulse[v] = 0;
            if (++s->vindex[v] >= len) s->vindex[v] = 0;
        }
        if (at)
            continue;

        const struct seq_step *st = &t->steps[index];
        if (!st->on)
            continue;
        /* r in 0..99 from the top 16 random bits, by multiply and shift. */
        if (st->probability < 100 &&
            (((next_random(s) >> 16) * 100u) >> 16) >= st->probability)
            continue;

        unsigned want = st->ratchet < 1 ? 1 : st->ratchet > 4 ? 4 : st->ratchet;
        unsigned hits = ratchet_hits[speed][want - 1];
        unsigned gap = ratchet_gap[speed][want - 1];
        /* Swing delays every second step by (swing - 50) % of two steps:
         * 2 * period * (swing - 50) / 100, as multiply and shift. */
        unsigned delay = 0;
        if ((index & 1) && t->swing > 50) {
            unsigned sw = t->swing > 75 ? 75 : t->swing;
            delay = (2u * period * (sw - 50u) * 656u + 32768u) >> 16;
            if (delay >= gap) delay = gap - 1;
        }
        if (delay == 0) {
            if (n < max && !t->mute)
                ev[n++] = (struct seq_event){ frame, (uint8_t)v, st->velocity };
            hits--;
            delay = gap;
        }
        s->pending_left[v] = (uint8_t)hits;
        s->pending_gap[v] = (uint8_t)gap;
        s->pending_wait[v] = (uint8_t)(delay - 1);
        s->pending_velocity[v] = st->velocity;
    }
    return n;
}

FM1_AUDIO_CODE unsigned seq_render(struct seq_state *s, const struct seq_pattern *p,
                    unsigned frames, struct seq_event *events, unsigned max)
{
    if (!s->playing)
        return 0;
    unsigned bpm = p->bpm100 < SEQ_BPM_MIN ? SEQ_BPM_MIN
                 : p->bpm100 > SEQ_BPM_MAX ? SEQ_BPM_MAX : p->bpm100;
    const uint32_t step = PHASE_STEP(bpm);
    const unsigned master = (p->length < 1 ? 1 : p->length > SEQ_STEPS ? SEQ_STEPS : p->length)
                            * SEQ_PULSES_PER_STEP;
    unsigned n = 0;
    for (unsigned f = 0; f < frames; f++) {
        if (s->phase >= PULSE_UNIT) {
            s->phase -= PULSE_UNIT;
            n = on_pulse(s, p, (uint16_t)f, events, n, max);
            if (++s->pulse >= master) {
                /* Pattern end: every voice restarts from its first step. */
                s->pulse = 0;
                for (unsigned v = 0; v < SEQ_VOICES; v++)   /* no memset: libc is in flash */
                    s->vpulse[v] = s->vindex[v] = 0;
            }
        }
        s->phase += step;
    }
    return n;
}
