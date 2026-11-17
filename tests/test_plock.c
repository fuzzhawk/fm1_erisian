/* deps: app/engine/plock.c app/engine/drums.c app/engine/seq.c app/engine/tables.c */
/* Parameter locks: the pool, its bookkeeping, and that a locked step really
 * plays its own sound through the sequencer and drum engine. */
#include "../app/engine/plock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct seq_pattern p;
static struct drum_kit kit;
static struct drums d;
static struct seq_state s;

/* Render frames through the sequencer and drums; returns a checksum of
 * the left channel so two renders can be compared. */
static uint64_t render_sum(unsigned frames)
{
    static int32_t out[2 * 128];
    uint64_t sum = 0;
    for (unsigned at = 0; at < frames; at += 128) {
        struct seq_event ev[32];
        memset(out, 0, sizeof out);
        unsigned n = seq_render(&s, &p, 128, ev, 32);
        drums_render(&d, out, 128, ev, n);
        for (unsigned f = 0; f < 128; f++) sum = sum * 31u + (uint32_t)out[2 * f];
    }
    return sum;
}

static void reset(void)
{
    seq_pattern_init(&p);
    drum_kit_default(&kit);
    drums_init(&d);
    drums_set_tempo(&d, &kit, p.bpm100);
    drums_prepare(&d, &kit);
}

int main(void)
{
    reset();
    /* A KS tom (voice 4) on step 1; lock its tune an octave-ish higher. */
    CHECK(kit.voice[4].engine == DRUM_KS);
    p.voices[4].steps[0].on = 1;
    seq_start(&s, 1);
    uint64_t base = render_sum(4096);
    CHECK(d.voice[4].x == &d.derived[4]);

    int locked;
    unsigned tune = plock_value(&p, &kit, 4, 0, PLOCK_E(KS_TUNE), &locked);
    CHECK(!locked && tune == kit.voice[4].e[KS_TUNE]);
    CHECK(plock_set(&p, &kit, &d, 4, 0, PLOCK_E(KS_TUNE), tune + 19) == 0);
    CHECK(plock_value(&p, &kit, 4, 0, PLOCK_E(KS_TUNE), &locked) == tune + 19 && locked);
    CHECK(p.voices[4].steps[0].lock == 1);
    CHECK(kit.voice[4].e[KS_TUNE] == tune);
    drums_init(&d);                                 /* fresh voices, same sound */
    drums_set_tempo(&d, &kit, p.bpm100);
    drums_prepare(&d, &kit);
    plock_prepare_all(&p, &kit, &d);
    seq_start(&s, 1);
    uint64_t higher = render_sum(4096);
    CHECK(d.voice[4].x == &d.locked[0]);            /* the step played the lock */
    CHECK(d.locked[0].period_q16 < d.derived[4].period_q16 * 2 / 3);   /* higher pitch */
    CHECK(higher != base);

    /* A common-chain lock (level) on another step of the same voice shares
     * nothing with step 1's slot. */
    CHECK(plock_set(&p, &kit, &d, 4, 8, PLOCK_C(DC_LEVEL), 10) == 0);
    CHECK(plock_slot(&p, 4, 8) == 1);
    CHECK(d.locked[1].level < d.derived[4].level);
    CHECK(d.locked[1].inc == d.derived[4].inc);    /* tune follows the kit */
    /* A second lock on the same step reuses its slot. */
    CHECK(plock_set(&p, &kit, &d, 4, 8, PLOCK_C(DC_PAN), 0) == 0);
    CHECK(plock_slot(&p, 4, 8) == 1 && plock_free_slots(&p) == SEQ_LOCK_SLOTS - 2);

    /* Kit edits reach the locked steps after plock_prepare_voice. */
    kit.voice[4].c[DC_DRIVE] = 100;
    drums_prepare_voice(&d, &kit, 4);
    plock_prepare_voice(&p, &kit, &d, 4);
    CHECK(d.locked[0].drive == d.derived[4].drive && d.locked[1].drive == d.derived[4].drive);

    /* Engine change: engine locks go (slot 0 had only tune: freed), common
     * locks stay. */
    drum_voice_engine_defaults(&kit.voice[4], DRUM_FM);
    drums_prepare_voice(&d, &kit, 4);
    plock_clear_engine(&p, &kit, &d, 4);
    CHECK(plock_slot(&p, 4, 0) < 0 && p.voices[4].steps[0].lock == 0);
    CHECK(plock_slot(&p, 4, 8) == 1 && d.locked[1].engine == DRUM_FM);
    CHECK(plock_value(&p, &kit, 4, 8, PLOCK_C(DC_LEVEL), &locked) == 10 && locked);

    /* Clearing a step frees its slot. */
    plock_clear_step(&p, 4, 8);
    CHECK(plock_slot(&p, 4, 8) < 0 && plock_free_slots(&p) == SEQ_LOCK_SLOTS);

    /* The pool fills up and refuses politely; existing locks still edit. */
    reset();
    for (unsigned i = 0; i < SEQ_LOCK_SLOTS; i++)
        CHECK(plock_set(&p, &kit, &d, i % SEQ_VOICES, i / SEQ_VOICES, PLOCK_C(DC_CUTOFF), 50) == 0);
    CHECK(plock_free_slots(&p) == 0);
    CHECK(plock_set(&p, &kit, &d, 10, 63, PLOCK_C(DC_CUTOFF), 50) == -1);
    CHECK(p.voices[10].steps[63].lock == 0);
    CHECK(plock_set(&p, &kit, &d, 0, 0, PLOCK_C(DC_RESO), 90) == 0);
    /* Bad indexes are refused. */
    CHECK(plock_set(&p, &kit, &d, 0, 0, 14, 1) == -1);
    CHECK(plock_set(&p, &kit, &d, 0, 0, PLOCK_C(DC_COUNT), 1) == -1);

    /* A damaged save: links that do not match both ways are dropped. */
    reset();
    CHECK(plock_set(&p, &kit, &d, 2, 3, PLOCK_C(DC_LEVEL), 5) == 0);
    CHECK(plock_set(&p, &kit, &d, 3, 4, PLOCK_C(DC_LEVEL), 5) == 0);
    p.voices[5].steps[5].lock = 2;             /* points at someone else's slot */
    p.voices[6].steps[6].lock = 200;           /* out of range */
    p.locks[7].voice = 9; p.locks[7].step = 9; /* slot with no step pointing at it */
    p.locks[8].voice = 40;                     /* nonsense owner */
    plock_validate(&p);
    CHECK(p.voices[5].steps[5].lock == 0 && p.voices[6].steps[6].lock == 0);
    CHECK(p.locks[7].voice == SEQ_UNLOCKED && p.locks[8].voice == SEQ_UNLOCKED);
    CHECK(plock_slot(&p, 2, 3) == 0 && plock_slot(&p, 3, 4) == 1);
    CHECK(plock_free_slots(&p) == SEQ_LOCK_SLOTS - 2);

    /* A lock event for a voice whose engine no longer matches the slot plays
     * the kit sound instead (stale slot). */
    reset();
    CHECK(plock_set(&p, &kit, &d, 4, 0, PLOCK_E(KS_TUNE), 100) == 0);
    drum_voice_engine_defaults(&kit.voice[4], DRUM_FM);
    drums_prepare_voice(&d, &kit, 4);             /* slot not re-prepared yet */
    drums_trigger(&d, 4, 100, 1);
    CHECK(d.voice[4].x == &d.derived[4]);
    drum_voice_engine_defaults(&kit.voice[4], DRUM_KS);
    drums_prepare_voice(&d, &kit, 4);
    drums_trigger(&d, 4, 100, 1);
    CHECK(d.voice[4].x == &d.locked[0]);
    drums_trigger(&d, 4, 100, 0);
    CHECK(d.voice[4].x == &d.derived[4]);

    if (failures) { printf("test_plock: %d failure(s)\n", failures); return 1; }
    printf("test_plock: ok\n");
    return 0;
}
