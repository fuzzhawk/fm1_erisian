/* SPDX-License-Identifier: MIT */
/* The audio interrupt: sequencer -> drum voices -> master. Everything here
 * runs from SRAM (FM1_AUDIO_CODE) and touches only RAM; no libc calls
 * (memset/memcpy live in flash). tools/check_audio_code.py checks the binary. */
#include "erisian.h"

#include "engine/mix.h"
#include "fm1.h"
#include "fm1_app.h"

struct seq_pattern pattern;
struct drum_kit kit;
struct drums drums;
struct seq_state seq;

/* Control -> audio requests. Single producer, single consumer: the control
 * task writes, then bumps the counter; the interrupt reads up to it. */
#define PAD_QUEUE 16u
static volatile uint8_t pad_voice[PAD_QUEUE], pad_velocity[PAD_QUEUE];
static volatile uint32_t pad_head;      /* written by the control task */
static uint32_t pad_tail;               /* audio interrupt */
static volatile uint32_t play_requests, stop_requests;
static uint32_t play_seen, stop_seen;
static volatile uint32_t blocks_done;
static volatile int32_t master_gain = 16384;   /* Q15 */

void audio_play(void) { play_requests++; }
void audio_stop(void) { stop_requests++; }
int audio_playing(void) { return seq.playing; }

void audio_pad(unsigned voice, unsigned velocity)
{
    uint32_t head = pad_head;
    if (head - pad_tail >= PAD_QUEUE)
        return; /* full: drop the hit */
    pad_voice[head % PAD_QUEUE] = (uint8_t)voice;
    pad_velocity[head % PAD_QUEUE] = (uint8_t)velocity;
    pad_head = head + 1;
}

void audio_set_master(uint16_t knob)
{
    /* Knob 0..1023 to gain 0..~0.75: headroom for 11 voices into the clipper. */
    master_gain = (int32_t)knob * 24;
}

void audio_wait_blocks(unsigned n)
{
    uint32_t first = blocks_done, t0 = fm1_millis();
    while (blocks_done - first < n && fm1_millis() - t0 < 50u) {}
}

void audio_set_sample(const int16_t *pcm, uint32_t frames)
{
    /* Stop readers first: render_voice checks pcm before every frame. */
    drums.sample.pcm = 0;
    audio_wait_blocks(2);
    drums.sample.frames = frames;
    drums.sample.pcm = pcm;
}

FM1_AUDIO_CODE int fm1_app_render(int32_t *out, unsigned frames)
{
    if (!out || frames != FM1_BLOCK_FRAMES)
        return -1;
    /* Volatile: a plain loop would be compiled into memset, which is in flash. */
    volatile int32_t *clear = out;
    for (unsigned i = 0; i < 2 * frames; i++)
        clear[i] = 0;

    if (stop_requests != stop_seen) { stop_seen = stop_requests; seq_stop(&seq); }
    if (play_requests != play_seen) {
        play_seen = play_requests;
        /* seq_start without memset: clear by hand. */
        seq.pulse = 0;
        seq.phase = 44100u * 60u * 100u; /* first pulse due at once */
        volatile uint8_t *vp = seq.vpulse, *vi = seq.vindex, *pl = seq.pending_left;
        volatile uint8_t *early = seq.early_step;
        for (unsigned v = 0; v < SEQ_VOICES; v++) {
            vp[v] = vi[v] = pl[v] = 0;
            early[v] = 0xFF;
        }
        if (!seq.random) seq.random = 0x23F00DD5u;
        seq.playing = 1;
    }

    struct seq_event ev[48];
    unsigned n = 0;
    while (pad_tail != pad_head && n < 16) {   /* live pads first, at frame 0 */
        ev[n].frame = 0;
        ev[n].voice = pad_voice[pad_tail % PAD_QUEUE];
        ev[n].velocity = pad_velocity[pad_tail % PAD_QUEUE];
        ev[n].lock = 0;
        n++;
        pad_tail++;
    }
    n += seq_render(&seq, &pattern, frames, ev + n, 48 - n);
    /* drums_render wants events in frame order: pads (frame 0) come first,
     * the sequencer's are already ordered. */
    drums_render(&drums, out, frames, ev, n);
    mix_master(out, frames, master_gain);
    blocks_done++;
    return 0;
}

FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    (void)output;
}
