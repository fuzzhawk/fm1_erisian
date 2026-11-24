/* SPDX-License-Identifier: MIT */
/* The audio interrupt: notes in, engine, master. Measures its own CPU use
 * with the platform's tick counter (the same one platform/audio.c uses). RAM
 * only, no libc, no division (tools/check_audio_code.py checks the binary). */
#include "mavis.h"

#include "board.h"
#include "fm1.h"
#include "fm1_app.h"
#include "fm1_device.h"

struct mavis_engine engine;
struct mv_patch patch;
struct mv_global global;

#define NOTE_QUEUE 32u
static volatile uint8_t note_key[NOTE_QUEUE], note_velocity[NOTE_QUEUE];
static volatile uint32_t note_head;     /* control task */
static uint32_t note_tail;              /* audio interrupt */
static volatile uint32_t off_requests;
static uint32_t off_seen;
static volatile uint8_t press_request;
static volatile int32_t master_gain = 16384;     /* Q15 */
static volatile uint32_t load_q16, load_peak_q16, active_voices;
static uint32_t inv_block_q32;          /* 2^32 / ticks per block */

void audio_init(void)
{
    uint64_t ticks = (uint64_t)fm1_device_clock() * FM1_BLOCK_FRAMES / 44118u;
    inv_block_q32 = ticks ? (uint32_t)((1ULL << 32) / ticks) : 0;
}

void audio_note(unsigned note, unsigned velocity)
{
    uint32_t head = note_head;
    if (head - note_tail >= NOTE_QUEUE) return;   /* full: audio_all_off catches strays */
    note_key[head % NOTE_QUEUE] = (uint8_t)note;
    note_velocity[head % NOTE_QUEUE] = (uint8_t)velocity;
    note_head = head + 1;
}

void audio_all_off(void) { off_requests++; }
void audio_press(int on) { press_request = (uint8_t)(on != 0); }
void audio_set_master(uint16_t knob) { master_gain = (int32_t)knob * 32; }   /* 0 .. ~1.0 */
int audio_sounding(void) { return active_voices != 0; }
unsigned audio_voices(void) { return active_voices; }
unsigned audio_load(void) { return (unsigned)((load_q16 * 100u) >> 16); }
unsigned audio_load_peak(int reset)
{
    unsigned p = (unsigned)((load_peak_q16 * 100u) >> 16);
    if (reset) load_peak_q16 = 0;
    return p;
}

FM1_AUDIO_CODE static inline uint32_t ticks_since(uint32_t from)
{
    uint32_t to = FM1_AUDIO_TICKS;
    return to >= from ? to - from : to + FM1_AUDIO_WRAP + 1u - from;
}

/* Cubic soft clip of a Q23 sample after the master gain. */
FM1_AUDIO_CODE static inline int32_t master_clip(int32_t x, int32_t gain)
{
    int32_t y = (int32_t)(((int64_t)x * gain) >> 15) >> 8;       /* Q15 */
    if (y > 49152) y = 49152;
    if (y < -49152) y = -49152;
    int32_t y3 = (int32_t)((((int64_t)y * y >> 15) * y) >> 15);
    y -= (y3 * 9709) >> 16;                                       /* y - 4/27 y^3 */
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    return y * 256;
}

FM1_AUDIO_CODE int fm1_app_render(int32_t *out, unsigned frames)
{
    const uint32_t t0 = FM1_AUDIO_TICKS;
    if (!out || frames != FM1_BLOCK_FRAMES) return -1;
    volatile int32_t *clear = out;      /* volatile: no memset call (flash) */
    for (unsigned i = 0; i < 2 * frames; i++) clear[i] = 0;

    if (off_requests != off_seen) { off_seen = off_requests; mv_all_off(&engine); }
    engine.pressing = press_request;
    for (unsigned k = 0; note_tail != note_head && k < 16; k++, note_tail++)
        mv_note(&engine, note_key[note_tail % NOTE_QUEUE], note_velocity[note_tail % NOTE_QUEUE]);

    mv_render(&engine, out, frames);

    const int32_t gain = master_gain;
    for (unsigned i = 0; i < 2 * frames; i++) out[i] = master_clip(out[i], gain);

    uint32_t n = 0;
    for (unsigned i = 0; i < MV_MAX_VOICES; i++) n += engine.voice[i].active;
    active_voices = n;

    /* CPU: this block's share of the block time (Q16), smoothed and peak. */
    uint32_t used = ticks_since(t0);
    uint32_t load = (uint32_t)(((uint64_t)used * inv_block_q32) >> 16);
    load_q16 = load_q16 + ((int32_t)(load - load_q16) >> 4);
    if (load > load_peak_q16) load_peak_q16 = load;
    return 0;
}

FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    (void)output;
}
