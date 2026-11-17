/* deps: app/engine/seq.c */
#include "../app/engine/seq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct hit { uint32_t frame; uint8_t voice, velocity, lock; };
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
            hits[nhits++] = (struct hit){ at + ev[i].frame, ev[i].voice, ev[i].velocity, ev[i].lock };
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

static void test_micro_and_locks(void)
{
    struct seq_pattern p;
    const double st = step_frames(12000);

    /* Late: +3 twelfths = a quarter step. */
    seq_pattern_init(&p);
    p.voices[0].steps[0].on = p.voices[0].steps[4].on = 1;
    p.voices[0].steps[4].micro = 3;
    run(&p, (uint32_t)(8 * st), 128, 1);
    CHECK(nhits == 2 && fabs(hits[1].frame - 4.25 * st) <= 1.0);

    /* Early: -6 = half a step before, fired from the step before. */
    p.voices[0].steps[4].micro = -6;
    run(&p, (uint32_t)(8 * st), 128, 1);
    CHECK(nhits == 2 && fabs(hits[1].frame - 3.5 * st) <= 1.0);
    /* ...also when the step before is the voice's last step at 1/2 speed. */
    p.voices[0].speed = SEQ_SPEED_1_2X;
    run(&p, (uint32_t)(10 * st), 128, 1);
    CHECK(nhits == 2 && fabs(hits[1].frame - 7.0 * st) <= 1.0);   /* step 4 at 8 st, -1 st */
    p.voices[0].speed = SEQ_SPEED_1X;

    /* Early on step 1 of the pattern: on time right after PLAY, early on
     * every later loop (from the end of the last step). */
    seq_pattern_init(&p);
    p.voices[1].steps[0].on = 1;
    p.voices[1].steps[0].micro = -3;
    run(&p, (uint32_t)(40 * st), 128, 1);
    CHECK(nhits == 3);
    if (nhits == 3) {
        CHECK(hits[0].frame == 0);
        CHECK(fabs(hits[1].frame - 15.75 * st) <= 1.0);
        CHECK(fabs(hits[2].frame - 31.75 * st) <= 1.0);
    }

    /* Swing and micro add: 75 % swing (+half step) and -3 (-quarter). */
    seq_pattern_init(&p);
    p.voices[2].swing = 75;
    p.voices[2].steps[1].on = 1;
    p.voices[2].steps[1].micro = -3;
    run(&p, (uint32_t)(4 * st), 128, 1);
    CHECK(nhits == 1 && fabs(hits[0].frame - 1.25 * st) <= 1.0);

    /* Block size does not change micro-timing. */
    seq_pattern_init(&p);
    for (unsigned i = 0; i < 16; i++) {
        p.voices[3].steps[i].on = 1;
        p.voices[3].steps[i].micro = (int8_t)((int)(i % 13) - 6);
    }
    run(&p, (uint32_t)(40 * st), 128, 1);
    static struct hit a[20000];
    unsigned na = nhits;
    memcpy(a, hits, sizeof a);
    CHECK(na > 30);
    run(&p, (uint32_t)(40 * st), 37, 1);
    CHECK(nhits == na);
    for (unsigned k = 0; k < na && k < nhits; k++) CHECK(hits[k].frame == a[k].frame);

    /* The lock slot rides along with every hit of the step. */
    seq_pattern_init(&p);
    p.voices[4].steps[2].on = 1;
    p.voices[4].steps[2].ratchet = 2;
    p.voices[4].steps[2].lock = 5;
    p.voices[4].steps[3].on = 1;
    run(&p, (uint32_t)(4 * st), 128, 1);
    CHECK(nhits == 3);
    if (nhits == 3) CHECK(hits[0].lock == 5 && hits[1].lock == 5 && hits[2].lock == 0);

    /* Old saves convert: fields kept, no micro, no locks. */
    static struct seq_pattern_v3 old;
    memset(&old, 0, sizeof old);
    old.length = 32;
    old.bpm100 = 9000;
    old.voices[5].length = 12;
    old.voices[5].swing = 60;
    old.voices[5].steps[7] = (struct seq_step_v3){ 1, 90, 50, 3 };
    seq_pattern_from_v3(&p, &old);
    CHECK(p.length == 32 && p.bpm100 == 9000 && p.voices[5].length == 12 && p.voices[5].swing == 60);
    CHECK(p.voices[5].steps[7].on == 1 && p.voices[5].steps[7].velocity == 90);
    CHECK(p.voices[5].steps[7].probability == 50 && p.voices[5].steps[7].ratchet == 3);
    CHECK(p.voices[5].steps[7].micro == 0 && p.voices[5].steps[7].lock == 0);
    for (unsigned i = 0; i < SEQ_LOCK_SLOTS; i++) CHECK(p.locks[i].voice == SEQ_UNLOCKED);
}

int main(void)
{
    test_timing();
    test_lengths_and_speed();
    test_swing_ratchet_probability();
    test_stop();
    test_micro_and_locks();
    if (failures) { printf("test_seq: %d failure(s)\n", failures); return 1; }
    printf("test_seq: ok\n");
    return 0;
}
