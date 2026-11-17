/* SPDX-License-Identifier: MIT */
#include "plock.h"

#include <string.h>

static int index_valid(unsigned i)
{
    return i < DRUM_EP || (i >= PLOCK_C(0) && i < PLOCK_C(DC_COUNT));
}

int plock_slot(const struct seq_pattern *p, unsigned voice, unsigned step)
{
    if (voice >= SEQ_VOICES || step >= SEQ_STEPS) return -1;
    unsigned l = p->voices[voice].steps[step].lock;
    return l && l <= SEQ_LOCK_SLOTS ? (int)l - 1 : -1;
}

static unsigned kit_value(const struct drum_voice_params *vp, unsigned i)
{
    return i < DRUM_EP ? vp->e[i] : vp->c[i - PLOCK_C(0)];
}

unsigned plock_value(const struct seq_pattern *p, const struct drum_kit *kit,
                     unsigned voice, unsigned step, unsigned index, int *locked)
{
    int slot = plock_slot(p, voice, step);
    *locked = 0;
    if (slot >= 0 && index < SEQ_LOCK_VALUES && p->locks[slot].value[index] != SEQ_UNLOCKED) {
        *locked = 1;
        return p->locks[slot].value[index];
    }
    return index_valid(index) ? kit_value(&kit->voice[voice], index) : 0;
}

static void prepare_slot(const struct seq_pattern *p, const struct drum_kit *kit,
                         struct drums *d, unsigned slot)
{
    const struct seq_lock *l = &p->locks[slot];
    if (l->voice >= SEQ_VOICES) return;
    struct drum_voice_params vp = kit->voice[l->voice];
    for (unsigned i = 0; i < SEQ_LOCK_VALUES; i++) {
        if (l->value[i] == SEQ_UNLOCKED || !index_valid(i)) continue;
        if (i < DRUM_EP) vp.e[i] = l->value[i];
        else vp.c[i - PLOCK_C(0)] = l->value[i];
    }
    drums_prepare_params(d, &vp, &d->locked[slot]);
}

static void free_slot(struct seq_pattern *p, unsigned slot)
{
    p->locks[slot].voice = SEQ_UNLOCKED;
    memset(p->locks[slot].value, SEQ_UNLOCKED, sizeof p->locks[slot].value);
}

int plock_set(struct seq_pattern *p, const struct drum_kit *kit, struct drums *d,
              unsigned voice, unsigned step, unsigned index, unsigned value)
{
    if (voice >= SEQ_VOICES || step >= SEQ_STEPS || !index_valid(index)) return -1;
    if (value > 127) value = 127;
    int slot = plock_slot(p, voice, step);
    if (slot < 0) {
        for (unsigned i = 0; i < SEQ_LOCK_SLOTS && slot < 0; i++)
            if (p->locks[i].voice == SEQ_UNLOCKED) slot = (int)i;
        if (slot < 0) return -1;
        free_slot(p, (unsigned)slot);
        p->locks[slot].step = (uint8_t)step;
        p->locks[slot].value[index] = (uint8_t)value;
        p->locks[slot].voice = (uint8_t)voice;
        prepare_slot(p, kit, d, (unsigned)slot);
        p->voices[voice].steps[step].lock = (uint8_t)(slot + 1);   /* last: now it plays */
        return 0;
    }
    p->locks[slot].value[index] = (uint8_t)value;
    prepare_slot(p, kit, d, (unsigned)slot);
    return 0;
}

void plock_clear_step(struct seq_pattern *p, unsigned voice, unsigned step)
{
    int slot = plock_slot(p, voice, step);
    if (voice < SEQ_VOICES && step < SEQ_STEPS)
        p->voices[voice].steps[step].lock = 0;            /* first: stop using it */
    if (slot >= 0) free_slot(p, (unsigned)slot);
}

void plock_clear_engine(struct seq_pattern *p, const struct drum_kit *kit, struct drums *d,
                        unsigned voice)
{
    for (unsigned s = 0; s < SEQ_LOCK_SLOTS; s++) {
        struct seq_lock *l = &p->locks[s];
        if (l->voice != voice) continue;
        int any = 0;
        for (unsigned i = 0; i < SEQ_LOCK_VALUES; i++) {
            if (i < DRUM_EP) l->value[i] = SEQ_UNLOCKED;
            if (l->value[i] != SEQ_UNLOCKED) any = 1;
        }
        if (any) prepare_slot(p, kit, d, s);
        else plock_clear_step(p, voice, l->step);
    }
}

void plock_prepare_voice(const struct seq_pattern *p, const struct drum_kit *kit,
                         struct drums *d, unsigned voice)
{
    for (unsigned s = 0; s < SEQ_LOCK_SLOTS; s++)
        if (p->locks[s].voice == voice) prepare_slot(p, kit, d, s);
}

void plock_prepare_all(const struct seq_pattern *p, const struct drum_kit *kit, struct drums *d)
{
    for (unsigned s = 0; s < SEQ_LOCK_SLOTS; s++) prepare_slot(p, kit, d, s);
}

void plock_tempo(const struct seq_pattern *p, const struct drum_kit *kit, struct drums *d)
{
    for (unsigned s = 0; s < SEQ_LOCK_SLOTS; s++) {
        unsigned v = p->locks[s].voice;
        if (v < SEQ_VOICES && (kit->voice[v].engine == DRUM_METAL || kit->voice[v].engine == DRUM_NOISE))
            prepare_slot(p, kit, d, s);
    }
}

void plock_validate(struct seq_pattern *p)
{
    for (unsigned v = 0; v < SEQ_VOICES; v++)
        for (unsigned s = 0; s < SEQ_STEPS; s++) {
            int slot = plock_slot(p, v, s);
            if (p->voices[v].steps[s].lock && (slot < 0 || p->locks[slot].voice != v ||
                                                p->locks[slot].step != s))
                p->voices[v].steps[s].lock = 0;
        }
    for (unsigned i = 0; i < SEQ_LOCK_SLOTS; i++) {
        struct seq_lock *l = &p->locks[i];
        if (l->voice == SEQ_UNLOCKED) continue;
        if (l->voice >= SEQ_VOICES || l->step >= SEQ_STEPS ||
            p->voices[l->voice].steps[l->step].lock != i + 1)
            free_slot(p, i);
    }
}

unsigned plock_free_slots(const struct seq_pattern *p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SEQ_LOCK_SLOTS; i++) n += p->locks[i].voice == SEQ_UNLOCKED;
    return n;
}
