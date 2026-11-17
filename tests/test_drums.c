/* deps: app/engine/drums.c app/engine/seq.c app/engine/tables.c app/engine/mix.c */
/* Drum voice tests, plus demo renders (build-host/demo.wav: a groove,
 * build-host/engines.wav: every engine one after another). */
#include "../app/engine/drums.h"
#include "../app/engine/mix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define FRAMES (44100 * 3)
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

static int32_t peak(unsigned from, unsigned to)
{
    int32_t p = 0;
    for (unsigned f = from; f < to; f++) {
        int32_t v = abs(buf[2 * f]);
        if (v > p) p = v;
    }
    return p;
}

static double energy(unsigned from, unsigned to)
{
    double e = 0;
    for (unsigned f = from; f < to; f++) e += (double)buf[2 * f] * buf[2 * f];
    return e / (to - from + 1);
}

static void setup(void)
{
    drums_init(&d);
    drum_kit_default(&kit);
    for (unsigned i = 0; i < 13553; i++)
        fake_kick[i] = (int16_t)(30000 * exp(-(double)i / 3000.0) *
                                 sin(2 * M_PI * 55 * i / 44100 * (1 + 2 * exp(-(double)i / 400.0))));
    d.sample.pcm = fake_kick;
    d.sample.frames = 13553;
    drums_set_tempo(&d, &kit, 12000);
    drums_prepare(&d, &kit);
}

static struct seq_event hit(unsigned v) { return (struct seq_event){ 0, (uint8_t)v, 127 }; }

/* Every engine with its defaults: audible, within range, and it ends. */
static void test_every_engine(void)
{
    for (unsigned e = DRUM_SAMPLE; e < DRUM_ENGINES; e++) {
        setup();
        drum_voice_engine_defaults(&kit.voice[4], e);
        drums_prepare(&d, &kit);
        struct seq_event h = hit(4);
        render(FRAMES, &h, 1);
        int32_t p = peak(0, FRAMES);
        CHECK(p > (1 << 18));
        CHECK(p <= (1 << 23));
        CHECK(!d.voice[4].active);       /* ended within 3 s */
        if (p <= (1 << 18) || d.voice[4].active)
            fprintf(stderr, "  engine %s: peak %d active %u\n", drum_engine_name(e), p, d.voice[4].active);
    }
    setup();
    for (unsigned v = 0; v < DRUM_VOICES; v++) {   /* the default kit */
        drums_init(&d); d.sample.pcm = fake_kick; d.sample.frames = 13553;
        drums_set_tempo(&d, &kit, 12000);
        drums_prepare(&d, &kit);
        struct seq_event h = hit(v);
        render(FRAMES, &h, 1);
        CHECK(peak(0, FRAMES) > (1 << 18) && peak(0, FRAMES) <= (1 << 23));
        if (peak(0, FRAMES) <= (1 << 18)) fprintf(stderr, "  kit voice %u quiet\n", v);
    }
}

/* KS: the pitch follows TUNE. */
static void test_ks_pitch(void)
{
    unsigned tunes[3] = { 30, 50, 70 };
    for (unsigned i = 0; i < 3; i++) {
        setup();
        drum_voice_engine_defaults(&kit.voice[4], DRUM_KS);
        kit.voice[4].e[KS_TUNE] = (uint8_t)tunes[i];
        kit.voice[4].e[KS_DECAY] = 110;
        kit.voice[4].e[KS_BEND] = 0;
        kit.voice[4].e[KS_NOISE] = 0;
        drums_prepare(&d, &kit);
        struct seq_event h = hit(4);
        render(44100, &h, 1);
        /* autocorrelation peak around the expected period */
        double hz = 20.0 * pow(100.0, tunes[i] / 127.0);
        int expect = (int)(44100.0 / hz + 0.5), best = 0;
        double bestv = -1e30;
        for (int lag = expect * 7 / 10; lag <= expect * 13 / 10; lag++) {
            double s = 0;
            for (int f = 4000; f < 12000; f++) s += (double)buf[2 * f] * buf[2 * (f + lag)];
            if (s > bestv) { bestv = s; best = lag; }
        }
        double got = 44100.0 / best;
        CHECK(fabs(got - hz) / hz < 0.04);
        if (fabs(got - hz) / hz >= 0.04) fprintf(stderr, "  KS tune %u: %.1f Hz, want %.1f\n", tunes[i], got, hz);
    }
}

/* METAL reverse: grows to a peak at the swell length, then stops. */
static void test_metal_reverse(void)
{
    setup();
    drum_voice_engine_defaults(&kit.voice[10], DRUM_METAL);
    kit.voice[10].e[MT_REVERSE] = 24;    /* 1 + 24*16/128 = 4 steps */
    drums_set_tempo(&d, &kit, 12000);
    drums_prepare(&d, &kit);
    struct seq_event h = hit(10);
    render(FRAMES, &h, 1);
    unsigned len = 4 * 5512;
    CHECK(energy(0, len / 4) < energy(3 * len / 4, len));  /* rising */
    CHECK(peak(len + 200, len + 4000) == 0);               /* cut after the swell */
    CHECK(peak(len - 2000, len) > (1 << 18));
}

/* NOISE rise lasts its length in steps at the tempo. */
static void test_noise_length(void)
{
    setup();
    drum_voice_engine_defaults(&kit.voice[6], DRUM_NOISE);
    kit.voice[6].e[NZ_MODE] = 0;      /* rise */
    kit.voice[6].e[NZ_LENGTH] = 14;   /* 1 + 7 = 8 steps */
    drums_set_tempo(&d, &kit, 12000);
    drums_prepare(&d, &kit);
    struct seq_event h = hit(6);
    render(FRAMES, &h, 1);
    unsigned len = 8 * 5512;
    CHECK(peak(len + 300, FRAMES) == 0);
    CHECK(peak(len - 3000, len) > peak(0, 3000));
}

/* Common chain: filter types, crushing, transient, migration. */
static void test_chain(void)
{
    /* High-pass on a low tom removes most energy; low-pass on a hat too. */
    setup();
    struct seq_event h = hit(4);
    render(22050, &h, 1);
    double plain = energy(0, 22050);
    kit.voice[4].c[DC_FTYPE] = 64; kit.voice[4].c[DC_CUTOFF] = 110;   /* HP high */
    drums_prepare(&d, &kit);
    render(22050, &h, 1);
    CHECK(energy(0, 22050) < plain / 4);

    setup();
    struct seq_event hh = hit(2);
    render(22050, &hh, 1);
    double hat = energy(0, 22050);
    kit.voice[2].c[DC_FTYPE] = 32; kit.voice[2].c[DC_CUTOFF] = 30;    /* LP low */
    drums_prepare(&d, &kit);
    render(22050, &hh, 1);
    CHECK(energy(0, 22050) < hat / 4);

    /* 4-bit crush: output takes few distinct values (per channel, pre-pan). */
    setup();
    kit.voice[1].c[DC_BITS] = 0;
    kit.voice[1].c[DC_PAN] = 64;
    drums_prepare(&d, &kit);
    struct seq_event sn = hit(1);
    render(4410, &sn, 1);
    int distinct = 0;
    int32_t seen[64];
    for (unsigned f = 0; f < 4410 && distinct < 60; f++) {
        int found = 0;
        for (int k = 0; k < distinct; k++) found |= seen[k] == buf[2 * f];
        if (!found) seen[distinct++] = buf[2 * f];
    }
    CHECK(distinct < 40);

    /* Transient CLICK adds energy at the very start of a voice that starts
     * quietly (the kick sample begins at zero). */
    setup();
    struct seq_event pad = hit(0);
    render(64, &pad, 1);
    int32_t before = peak(0, 8);
    kit.voice[0].c[DC_TRANS] = 32; kit.voice[0].c[DC_TLEVEL] = 127;
    drums_prepare(&d, &kit);
    render(64, &pad, 1);
    CHECK(peak(0, 8) > before);

    /* v1 kits convert: engines and the FM parameters keep their values. */
    struct drum_kit_v1 old;
    memset(&old, 0, sizeof old);
    old.voice[1].engine = DRUM_FM;
    old.voice[1].choke = 2;
    for (unsigned i = 0; i < 13; i++) old.voice[1].p[i] = (uint8_t)(10 + i);
    old.voice[0].engine = DRUM_SAMPLE;
    struct drum_kit k2;
    drum_kit_from_v1(&k2, &old);
    CHECK(k2.voice[1].engine == DRUM_FM && k2.voice[1].choke == 2);
    CHECK(k2.voice[1].c[DC_LEVEL] == 10 && k2.voice[1].c[DC_PAN] == 11);
    CHECK(k2.voice[1].e[FM_TUNE] == 12 && k2.voice[1].e[FM_NTONE] == 22);
    CHECK(k2.voice[1].e[FM_RATIO] == 4);   /* v1 p[6] = 16: index 16 & 15 = 0 -> 0*8+4 */
    CHECK(k2.voice[1].c[DC_FTYPE] == 0 && k2.voice[1].c[DC_BITS] == 127);
    CHECK(k2.voice[0].engine == DRUM_SAMPLE);
}

static void test_choke_timing(void)
{
    setup();
    struct seq_event e = { 300, 1, 127 };
    render(1000, &e, 1);
    CHECK(peak(0, 300) == 0);
    CHECK(peak(300, 310) > 0);
    struct seq_event two[2] = { { 0, 3, 127 }, { 2000, 2, 127 } };
    setup();
    render(4000, two, 2);
    CHECK(!d.voice[3].active);
    setup();
    struct seq_event open_only = { 0, 3, 127 };
    render(4000, &open_only, 1);
    CHECK(d.voice[3].active);
    /* Sample removed while ringing: stops instead of reading NULL. */
    setup();
    struct seq_event k = hit(0);
    render(500, &k, 1);
    d.sample.pcm = NULL;
    render(1000, NULL, 0);
    CHECK(!d.voice[0].active);
}

static void test_master(void)
{
    static int32_t b[2 * 1000];
    for (int i = 0; i < 2000; i++) b[i] = (i - 1000) * 40000;
    mix_master(b, 1000, 32768);
    for (int i = 0; i < 2000; i++) CHECK(b[i] <= MIX_FULL_SCALE && b[i] >= -MIX_FULL_SCALE);
    for (int i = 1; i < 2000; i++) CHECK(b[i] >= b[i - 1]);
}

static void write_wav(const char *path, const int32_t *mix, unsigned frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t bytes = frames * 4, riff = 36 + bytes;
    uint8_t h[41] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data";
    memcpy(h + 4, &riff, 4);
    fwrite(h, 1, 40, f);
    fwrite(&bytes, 4, 1, f);
    for (unsigned i = 0; i < 2 * frames; i++) {
        int32_t v = mix[i] >> 8;
        int16_t o = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        fwrite(&o, 2, 1, f);
    }
    fclose(f);
}

static int32_t mixbuf[2 * 44100 * 13];

/* A 2-bar groove with the default kit through the sequencer. */
static void demo(const char *path)
{
    struct seq_pattern p;
    struct seq_state st;
    seq_pattern_init(&p);
    p.bpm100 = 11800;
    const char *rows[DRUM_VOICES] = {
        "x...x...x..x.x..", "....x.......x...", "x.x.x.x.x.x.x.xx", "......x.......x.",
        "..........x.....", "...........x..x.", "............x...", "..x.....x.....x.",
        "x.xxx.xxx.xxx.xx", "x...............", "............x...",
    };
    for (unsigned v = 0; v < DRUM_VOICES; v++)
        for (unsigned s = 0; s < 16; s++)
            p.voices[v].steps[s].on = rows[v][s] == 'x';
    p.voices[2].steps[15].ratchet = 3;
    p.voices[2].swing = 58;
    p.voices[8].swing = 58;
    for (unsigned s = 1; s < 16; s += 2) p.voices[8].steps[s].velocity = 60;
    setup();
    drums_set_tempo(&d, &kit, p.bpm100);
    seq_start(&st, 23);
    unsigned total = (unsigned)(44100.0 * 60 / 118 * 8);
    memset(mixbuf, 0, sizeof mixbuf);
    for (unsigned at = 0; at < total; at += 128) {
        struct seq_event ev[32];
        unsigned n = seq_render(&st, &p, 128, ev, 32);
        drums_render(&d, mixbuf + 2 * at, 128, ev, n);
        mix_master(mixbuf + 2 * at, 128, 20000);
    }
    write_wav(path, mixbuf, total);
    printf("  demo: %s (%.1f s)\n", path, total / 44100.0);
}

/* Every engine, defaults, one per second (FM ... NOISE modes). */
static void engines(const char *path)
{
    setup();
    memset(mixbuf, 0, sizeof mixbuf);
    unsigned slot = 0;
    struct { unsigned engine; uint8_t tweak_i, tweak_v; } list[] = {
        { DRUM_SAMPLE, 0, 64 }, { DRUM_FM, 0, 62 }, { DRUM_KS, 0, 40 }, { DRUM_KS, KS_EXCITE, 100 },
        { DRUM_METAL, 0, 70 }, { DRUM_METAL, MT_REVERSE, 24 }, { DRUM_SHAKER, 0, 100 },
        { DRUM_OBJECT, OB_MATERIAL, 0 }, { DRUM_OBJECT, OB_MATERIAL, 60 }, { DRUM_OBJECT, OB_CRUNCH, 90 },
        { DRUM_NOISE, NZ_MODE, 0 }, { DRUM_NOISE, NZ_MODE, 100 },
    };
    for (unsigned i = 0; i < sizeof list / sizeof list[0]; i++) {
        drum_voice_engine_defaults(&kit.voice[4], list[i].engine);
        kit.voice[4].e[list[i].tweak_i] = list[i].tweak_v;
        drums_prepare(&d, &kit);
        struct seq_event h = hit(4);
        unsigned at0 = slot * 44100;
        for (unsigned at = 0; at < 44100; at += 128) {
            drums_render(&d, mixbuf + 2 * (at0 + at), 128, at == 0 ? &h : NULL, at == 0);
            mix_master(mixbuf + 2 * (at0 + at), 128, 24000);
        }
        d.voice[4].active = 0;
        slot++;
    }
    write_wav(path, mixbuf, slot * 44100);
    printf("  engines: %s (%u one-shots, 1 s each)\n", path, slot);
}

int main(void)
{
    test_every_engine();
    test_ks_pitch();
    test_metal_reverse();
    test_noise_length();
    test_chain();
    test_choke_timing();
    test_master();
    demo("build-host/demo.wav");
    engines("build-host/engines.wav");
    if (failures) { printf("test_drums: %d failure(s)\n", failures); return 1; }
    printf("test_drums: ok\n");
    return 0;
}
