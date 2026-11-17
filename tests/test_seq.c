/* deps: app/engine/seq.c */
#include "../app/engine/seq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct hit { uint32_t frame; uint8_t voice, velocity; };
static struct hit hits[20000];
static unsigned nhits;

/* Run for total frames in blocks of `block`, collecting absolute frames. */
static void run(const struct seq_pattern *p, uint32_t total, unsigned block, uint32_t seed)
{
    struct seq_state s;
    struct seq_event ev[64];
    seq_start(&s, seed);
    nhits = 0;
    for (uint32_t at = 0; at < total; at += block) {
        unsigned n = total - at < block ? total - at : block;
        unsigned k = seq_render(&s, p, n, ev, 64);
        for (unsigned i = 0; i < k && nhits < 20000; i++)
            hits[nhits++] = (struct hit){ at + ev[i].frame, ev[i].voice, ev[i].velocity };
    }
}

static unsigned count_voice(unsigned v)
{
    unsigned c = 0;
    for (unsigned i = 0; i < nhits; i++) c += hits[i].voice == v;
    return c;
}

static double step_frames(unsigned bpm100) { return 44100.0 * 60.0 * 100.0 / (bpm100 * 4.0); }

static void test_timing(void)
{
    struct seq_pattern p;
    seq_pattern_init(&p);
    for (unsigned s = 0; s < 16; s++) p.voices[0].steps[s].on = 1;
    const uint32_t total = 44100 * 8;
    run(&p, total, 128, 1);
    CHECK(nhits == 64); /* 120 BPM: 8 s = 16 beats = 64 sixteenths */
    for (unsigned k = 0; k < nhits; k++)
        CHECK(fabs(hits[k].frame - k * step_frames(12000)) <= 1.0);

    /* Block size must not change timing. */
    static struct hit a[20000];
    unsigned na = nhits;
    memcpy(a, hits, sizeof a);
    unsigned sizes[] = { 1, 37, 64, 256, 1000 };
    for (unsigned i = 0; i < 5; i++) {
        run(&p, total, sizes[i], 1);
        CHECK(nhits == na);
        for (unsigned k = 0; k < na && k < nhits; k++) CHECK(hits[k].frame == a[k].frame);
    }

    /* Odd tempo: 133.37 BPM, still within a frame. */
    p.bpm100 = 13337;
    run(&p, total, 128, 1);
    for (unsigned k = 0; k < nhits; k++)
        CHECK(fabs(hits[k].frame - k * step_frames(13337)) <= 1.0);
    CHECK(nhits == (unsigned)floor(total / step_frames(13337)) + 1);
}

static void test_lengths_and_speed(void)
{
    struct seq_pattern p;
    seq_pattern_init(&p);
    uint32_t bar = (uint32_t)(16 * step_frames(12000));

    /* Voice 1: length 3, trig on its first step; master 16 resets it. */
    p.voices[1].length = 3;
    p.voices[1].steps[0].on = 1;
    run(&p, 2 * bar, 128, 1);
    unsigned expect[] = { 0, 3, 6, 9, 12, 15, 16, 19, 22, 25, 28, 31 };
    CHECK(count_voice(1) == 12);
    for (unsigned i = 0, j = 0; i < nhits; i++) {
        if (hits[i].voice != 1) continue;
        CHECK(fabs(hits[i].frame - expect[j] * step_frames(12000)) <= 1.0);
        j++;
    }

    /* Speeds: all 64 steps on, length 64; one 16-step bar. */
    seq_pattern_init(&p);
    unsigned expect_hits[SEQ_SPEED_COUNT] = { 32, 16, 12, 8, 4 };
    for (unsigned sp = 0; sp < SEQ_SPEED_COUNT; sp++) {
        p.voices[2].speed = (uint8_t)sp;
        p.voices[2].length = 64;
        for (unsigned s = 0; s < 64; s++) p.voices[2].steps[s].on = 1;
        run(&p, bar - 10, 128, 1);
        CHECK(count_voice(2) == expect_hits[sp]);
    }

    /* Master length 5: voice with 16 steps restarts every 5 steps. */
    seq_pattern_init(&p);
    p.length = 5;
    p.voices[3].steps[0].on = 1;
    p.voices[3].steps[7].on = 1; /* never reached */
    run(&p, 2 * bar, 128, 1);
    CHECK(count_voice(3) == 7); /* steps 0,5,10,...,30 */
}

static void test_swing_ratchet_probability(void)
{
    struct seq_pattern p;
    seq_pattern_init(&p);
    double st = step_frames(12000);
    p.voices[0].swing = 75;
    p.voices[0].steps[0].on = 1;
    p.voices[0].steps[1].on = 1;
    run(&p, (uint32_t)(2 * st), 128, 1);
    CHECK(nhits == 2);
    CHECK(nhits == 2 && fabs(hits[1].frame - 1.5 * st) <= 1.0); /* dotted feel */

    seq_pattern_init(&p);
    p.voices[4].steps[0].on = 1;
    p.voices[4].steps[0].ratchet = 4;
    p.voices[4].steps[0].velocity = 77;
    run(&p, (uint32_t)st, 128, 1);
    CHECK(nhits == 4);
    for (unsigned k = 0; k < nhits; k++) {
        CHECK(fabs(hits[k].frame - k * st / 4) <= 1.0);
        CHECK(hits[k].velocity == 77);
    }

    /* Ratchet 3 at 2x (period 6 pulses): 3 hits of 2 pulses. */
    p.voices[4].speed = SEQ_SPEED_2X;
    p.voices[4].steps[0].ratchet = 3;
    run(&p, (uint32_t)(st / 2), 128, 1);
    CHECK(nhits == 3);

    seq_pattern_init(&p);
    for (unsigned s = 0; s < 16; s++) { p.voices[5].steps[s].on = 1; p.voices[5].steps[s].probability = 0; }
    run(&p, (uint32_t)(64 * st), 128, 1);
    CHECK(nhits == 0);
    for (unsigned s = 0; s < 16; s++) p.voices[5].steps[s].probability = 50;
    run(&p, (uint32_t)(400 * st), 128, 7);
    CHECK(nhits > 150 && nhits < 250);
    unsigned first = nhits;
    run(&p, (uint32_t)(400 * st), 128, 7);
    CHECK(nhits == first); /* same seed, same pattern */

    p.voices[5].mute = 1;
    run(&p, (uint32_t)(64 * st), 128, 7);
    CHECK(nhits == 0);
}

static void test_stop(void)
{
    struct seq_pattern p;
    struct seq_state s;
    struct seq_event ev[8];
    seq_pattern_init(&p);
    p.voices[0].steps[0].on = 1;
    seq_start(&s, 1);
    CHECK(seq_render(&s, &p, 128, ev, 8) == 1);
    seq_stop(&s);
    CHECK(seq_render(&s, &p, 100000, ev, 8) == 0);
}

int main(void)
{
    test_timing();
    test_lengths_and_speed();
    test_swing_ratchet_probability();
    test_stop();
    if (failures) { printf("test_seq: %d failure(s)\n", failures); return 1; }
    printf("test_seq: ok\n");
    return 0;
}
