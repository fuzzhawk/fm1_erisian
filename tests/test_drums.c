/* deps: app/engine/drums.c app/engine/seq.c app/engine/tables.c app/engine/mix.c */
/* Drum voice tests, plus a demo render (build-host/demo.wav) of a pattern
 * through the sequencer, so the result can be heard before flashing. */
#include "../app/engine/drums.h"
#include "../app/engine/mix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define FRAMES 44100
static int32_t buf[2 * FRAMES];
static struct drums d;
static struct drum_kit kit;
static int16_t fake_kick[13553];

static void render(unsigned frames, const struct seq_event *ev, unsigned n)
{
    memset(buf, 0, sizeof buf);
    for (unsigned at = 0; at < frames; at += 128) {
        struct seq_event local[8];
        unsigned k = 0;
        for (unsigned i = 0; i < n; i++)
            if (ev[i].frame >= at && ev[i].frame < at + 128 && k < 8) {
                local[k] = ev[i];
                local[k++].frame = (uint16_t)(ev[i].frame - at);
            }
        drums_render(&d, buf + 2 * at, frames - at < 128 ? frames - at : 128, local, k);
    }
}

static int32_t peak(unsigned from, unsigned to, unsigned ch)
{
    int32_t p = 0;
    for (unsigned f = from; f < to; f++) {
        int32_t v = buf[2 * f + ch] < 0 ? -buf[2 * f + ch] : buf[2 * f + ch];
        if (v > p) p = v;
    }
    return p;
}

static void setup(void)
{
    drums_init(&d);
    drum_kit_default(&kit);
    for (unsigned i = 0; i < 13553; i++)
        fake_kick[i] = (int16_t)(30000 * exp(-(double)i / 3000.0) * sin(2 * M_PI * 55 * i / 44100 * (1 + 2 * exp(-(double)i / 400.0))));
    d.sample.pcm = fake_kick;
    d.sample.frames = 13553;
    drums_prepare(&d, &kit);
}

static void test_every_voice(void)
{
    setup();
    for (unsigned v = 0; v < DRUM_VOICES; v++) {
        drums_init(&d);
        d.sample.pcm = fake_kick; d.sample.frames = 13553;
        drums_prepare(&d, &kit);
        struct seq_event e = { 0, (uint8_t)v, 127 };
        render(FRAMES, &e, 1);
        int32_t p = peak(0, 4410, 0);
        CHECK(p > (1 << 18));             /* clearly audible */
        CHECK(p <= (1 << 23));            /* within Q23 */
        if (v != 10 && v != 0)            /* all but the long crash end in 1 s */
            CHECK(!d.voice[v].active);
        if (p <= (1 << 18)) fprintf(stderr, "  voice %u peak %d\n", v, p);
    }
}

static void test_sample_exact_and_pitch(void)
{
    setup();
    struct seq_event e = { 0, 0, 127 };
    render(2000, &e, 1);
    /* Original speed, full gain: output tracks the sample within rounding. */
    int32_t gain = d.voice[0].gain;
    for (unsigned f = 1; f < 200; f++) {
        int32_t expect = ((int32_t)fake_kick[f] * gain >> 15) * 256;
        CHECK(abs(buf[2 * f] - expect) < (4 << 8) + abs(expect) / 100);
    }
    kit.voice[0].p[DP_TUNE] = 127;
    drums_prepare_voice(&d, &kit, 0);
    CHECK(abs((int)d.derived[0].sample_step - 2 * 65536) < 2 * 65536 / 20);  /* within 5 % */
    kit.voice[0].p[DP_TUNE] = 0;
    drums_prepare_voice(&d, &kit, 0);
    CHECK(abs((int)d.derived[0].sample_step - 32768) < 32768 / 20);

    /* Sample removed while the kick rings (e.g. a new upload): the voice
     * stops instead of reading through NULL. Then new hits stay silent. */
    render(500, &e, 1);
    d.sample.pcm = NULL;
    render(1000, NULL, 0);
    CHECK(!d.voice[0].active);
    render(1000, &e, 1);
    CHECK(peak(0, 1000, 0) == 0);
}

static void test_timing_choke_pan(void)
{
    setup();
    struct seq_event e = { 300, 1, 127 }; /* snare: noise, audible at once */
    render(1000, &e, 1);
    CHECK(peak(0, 300, 0) == 0);
    CHECK(peak(300, 310, 0) > 0);

    setup();
    struct seq_event two[2] = { { 0, 3, 127 }, { 2000, 2, 127 } }; /* open, then closed hat */
    render(4000, two, 2);
    CHECK(!d.voice[3].active);              /* choked */
    struct seq_event open_only = { 0, 3, 127 };
    setup();
    render(4000, &open_only, 1);
    CHECK(d.voice[3].active);               /* still ringing without the choke */

    setup();
    kit.voice[1].p[DP_PAN] = 0;             /* hard left */
    drums_prepare(&d, &kit);
    struct seq_event s = { 0, 1, 127 };
    render(2000, &s, 1);
    CHECK(peak(0, 2000, 0) > 0 && peak(0, 2000, 1) == 0);

    /* Velocity scales the level. */
    setup();
    struct seq_event loud = { 0, 9, 127 }, soft = { 0, 9, 32 };
    render(4410, &loud, 1);
    int32_t pl = peak(0, 4410, 0);
    drums_init(&d); d.sample.pcm = fake_kick; d.sample.frames = 13553; drums_prepare(&d, &kit);
    render(4410, &soft, 1);
    int32_t ps = peak(0, 4410, 0);
    CHECK(ps > pl / 6 && ps < pl / 3);
}

/* A 2-bar groove through the sequencer, mixed and clipped like the firmware. */
static void demo(const char *path)
{
    static int32_t mix[2 * 44100 * 5];
    struct seq_pattern p;
    struct seq_state st;
    seq_pattern_init(&p);
    p.bpm100 = 11800;
    const char *rows[DRUM_VOICES] = {
        "x...x...x..x.x..", /* kick */
        "....x.......x...", /* snare */
        "x.x.x.x.x.x.x.xx", /* closed hat */
        "......x.......x.", /* open hat (choked by the closed hat) */
        "..........x.....", "...........x..x.",
        "............x...", "..x.....x.....x.",
        "...x...........x", "x...............", "x...............",
    };
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        for (unsigned s = 0; s < 16; s++)
            p.voices[v].steps[s].on = rows[v][s] == 'x';
    p.voices[2].steps[15].ratchet = 3;
    p.voices[2].swing = 58;
    for (unsigned s = 1; s < 16; s += 2) p.voices[2].steps[s].velocity = 70;
    p.voices[7].steps[14].probability = 50;
    setup();
    seq_start(&st, 23);
    unsigned total = (unsigned)(44100.0 * 60 / 118 * 8) ; /* 2 bars */
    memset(mix, 0, sizeof mix);
    for (unsigned at = 0; at < total; at += 128) {
        struct seq_event ev[32];
        unsigned n = seq_render(&st, &p, 128, ev, 32);
        drums_render(&d, mix + 2 * at, 128, ev, n);
        mix_master(mix + 2 * at, 128, 20000); /* ~0.6: headroom for 11 voices */
    }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t bytes = total * 4;
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data";
    uint32_t riff = 36 + bytes;
    memcpy(h + 4, &riff, 4);
    fwrite(h, 1, 40, f);
    fwrite(&bytes, 4, 1, f);
    int32_t top = 0;
    for (unsigned i = 0; i < 2 * total; i++) {
        int32_t v = mix[i] >> 8;
        if (abs(v) > top) top = abs(v);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        int16_t o = (int16_t)v;
        fwrite(&o, 2, 1, f);
    }
    fclose(f);
    CHECK(top > 8000);
    CHECK(top <= 32767);
    printf("  demo: %s (%.1f s, peak %d of 32767%s)\n", path, total / 44100.0, top,
           top > 32767 ? ", clipped" : "");
}

static void test_master(void)
{
    static int32_t b[2 * 1000];
    for (int i = 0; i < 2000; i++) b[i] = (i - 1000) * 40000;    /* up to ~4.8x FS */
    mix_master(b, 1000, 32768);
    for (int i = 0; i < 2000; i++) CHECK(b[i] <= MIX_FULL_SCALE && b[i] >= -MIX_FULL_SCALE);
    for (int i = 1; i < 2000; i++) CHECK(b[i] >= b[i - 1]);       /* monotonic */
    int32_t q[2] = { 1 << 19, -(1 << 19) };                        /* 1/16 FS: ~linear */
    mix_master(q, 1, 32768);
    CHECK(abs(q[0] - (1 << 19)) < (1 << 19) / 100 && abs(q[1] + q[0]) <= 2);
    int32_t h[2] = { 1 << 22, 0 };
    mix_master(h, 1, 16384);                                       /* gain 0.5 */
    CHECK(abs(h[0] - (1 << 21)) < (1 << 21) / 50);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_every_voice();
    test_sample_exact_and_pitch();
    test_timing_choke_pan();
    test_master();
    demo("build-host/demo.wav");
    if (failures) { printf("test_drums: %d failure(s)\n", failures); return 1; }
    printf("test_drums: ok\n");
    return 0;
}
