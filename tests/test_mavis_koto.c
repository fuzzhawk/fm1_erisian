/* deps: mavis/engine/engine.c mavis/engine/mtables.c */
/* MAVIS KOTO: in tune across the range (incl. pitch-dependent loop delay
 * compensation), decays and frees voices, re-plucks, steals cleanly, bends,
 * tunings and key maps. Writes build-host/mavis_koto.wav. */
#include "../mavis/engine/engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct mavis_engine e;
static struct mv_patch patch;
static struct mv_global glob;
#define MAXF (44100 * 6)
static int32_t buf[2 * MAXF];

static void render(unsigned frames)
{
    memset(buf, 0, sizeof buf);
    for (unsigned at = 0; at + 128 <= frames; at += 128) mv_render(&e, buf + 2 * at, 128);
}

static double rms(unsigned from, unsigned to)
{
    double s = 0;
    for (unsigned f = from; f < to; f++) { double a = (buf[2 * f] + buf[2 * f + 1]) / 2 / 8388608.0; s += a * a; }
    return sqrt(s / (to - from));
}

/* Fundamental by normalised autocorrelation (first strong peak). */
static double pitch(unsigned from, unsigned n)
{
    static double c[2200];
    double best = 0;
    for (unsigned lag = 10; lag < 2100; lag++) {
        double acc = 0, e0 = 0, e1 = 0;
        for (unsigned i = 0; i < n; i++) {
            double a = buf[2 * (from + i)] + buf[2 * (from + i) + 1];
            double b = buf[2 * (from + i + lag)] + buf[2 * (from + i + lag) + 1];
            acc += a * b; e0 += a * a; e1 += b * b;
        }
        c[lag] = e0 > 0 && e1 > 0 ? acc / sqrt(e0 * e1) : 0;
        if (c[lag] > best) best = c[lag];
    }
    for (unsigned lag = 11; lag < 2099; lag++)
        if (c[lag] > 0.85 * best && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
            double a = c[lag - 1], b = c[lag], d = c[lag + 1];
            double off = (a - d) / (2 * (a - 2 * b + d));
            return 44100.0 / (lag + (fabs(off) < 1 ? off : 0));
        }
    return 0;
}

static void fresh(void)
{
    mv_init(&e);
    mv_patch_default(&patch, MV_KOTO);
    mv_global_default(&glob);
    patch.p[KT_HUMAN] = 0;            /* deterministic pitch for the checks */
    patch.p[KT_TENSION] = 0;
    mv_prepare(&e, &patch, &glob);
}

static unsigned active(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MV_MAX_VOICES; i++) n += e.voice[i].active;
    return n;
}

static double hz(unsigned note) { return 440.0 * pow(2.0, ((int)note - 69) / 12.0); }

static void test_tuning(void)
{
    /* In tune from low to high, with and without stiffness (dispersion
     * shifts partials but the fundamental must stay within a few cents). */
    static const unsigned notes[] = { 38, 45, 50, 57, 62, 69, 74, 81, 88 };
    for (unsigned stiff = 0; stiff < 2; stiff++)
        for (unsigned i = 0; i < sizeof notes / sizeof notes[0]; i++) {
            fresh();
            patch.p[KT_STIFF] = stiff ? 40 : 0;
            patch.p[KT_SAWARI] = 0;
            mv_prepare(&e, &patch, &glob);
            mv_note(&e, notes[i], 100);
            render(22050);
            double f = pitch(4096, 4096), want = hz(notes[i]);
            double cents = f > 0 ? 1200 * log2(f / want) : 9999;
            if (fabs(cents) > 6)
                fprintf(stderr, "  note %u stiff %u: %.2f Hz (want %.2f), %.1f cents\n",
                        notes[i], stiff, f, want, cents);
            CHECK(fabs(cents) < (stiff ? 10 : 5));
            CHECK(rms(0, 4096) > 0.005);
        }
}

static void test_life(void)
{
    /* Decays on its own and frees the voice; key up damps it sooner. */
    fresh();
    mv_note(&e, 57, 100);
    render(44100);
    double early = rms(0, 4410), late = rms(39690, 44100);
    CHECK(early > 0.01 && late < early);
    mv_note(&e, 57, 0);
    render(44100 * 6);
    CHECK(active() == 0);

    fresh();
    patch.p[KT_DAMP] = 120;
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 57, 100);
    render(4410);
    mv_note(&e, 57, 0);
    render(44100);
    CHECK(rms(22050, 44100) < 0.0005);

    /* Re-plucking the same note keeps one voice. */
    fresh();
    mv_note(&e, 60, 100);
    render(4410);
    mv_note(&e, 60, 100);
    CHECK(active() == 1);

    /* Stealing: more notes than voices; the stolen one fades for a block,
     * then the new note sounds; no voice count overflow. */
    fresh();
    for (unsigned i = 0; i < glob.voices + 3u; i++) { mv_note(&e, 50 + 2 * i, 100); render(256); }
    CHECK(active() == glob.voices);
    unsigned newest = 0;
    for (unsigned i = 0; i < glob.voices; i++) newest += e.voice[i].note >= 50 + 2 * 3;
    CHECK(newest == glob.voices);
    /* Output stays bounded with every voice ringing. */
    double peak = 0;
    for (unsigned f = 0; f < 256; f++) { double a = fabs(buf[2 * f] / 8388608.0); if (a > peak) peak = a; }
    CHECK(peak < 4.0);
}

static void test_bends(void)
{
    /* Oshide: holding the press raises held notes by the set amount. */
    fresh();
    patch.p[KT_OSHIDE] = 64;          /* ~1 semitone */
    patch.p[KT_PRESS] = 0;            /* fast */
    patch.p[KT_DECAY] = 127;
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 57, 100);
    render(8192);
    e.pressing = 1;
    render(22050);
    double f = pitch(8192, 4096);
    double semis = 12 * log2(f / hz(57));
    CHECK(fabs(semis - 64 * 1032 / 65536.0) < 0.1);
    e.pressing = 0;
    render(22050);
    CHECK(fabs(1200 * log2(pitch(8192, 4096) / hz(57))) < 6);

    /* Bend-in: starts low, arrives at pitch. */
    fresh();
    patch.p[KT_BENDIN] = 64;
    patch.p[KT_BENDTIME] = 20;
    patch.p[KT_DECAY] = 127;
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 57, 100);
    CHECK(e.voice[0].bendin_q16 < -60000);
    render(44100);
    CHECK(fabs(1200 * log2(pitch(30000, 4096) / hz(57))) < 6);
}

static void test_keys_and_tunings(void)
{
    struct mv_global g;
    mv_global_default(&g);
    g.map = MAP_KEYS;
    CHECK(mv_key_note(&g, 0, 0) == 53 && mv_key_note(&g, 26, 0) == 79 && mv_key_note(&g, 0, 1) == 65);
    /* White keys play hirajoshi from D3: D E F A Bb D...; black = +1. */
    g.map = MAP_WHITE;
    g.scale = SC_HIRAJOSHI;
    g.root = 2;
    CHECK(mv_key_note(&g, 0, 0) == 50);     /* F key -> D3 */
    CHECK(mv_key_note(&g, 2, 0) == 52);     /* E3 */
    CHECK(mv_key_note(&g, 4, 0) == 53);     /* F3 */
    CHECK(mv_key_note(&g, 6, 0) == 57);     /* A3 */
    CHECK(mv_key_note(&g, 7, 0) == 58);     /* Bb3 */
    CHECK(mv_key_note(&g, 9, 0) == 62);     /* D4 */
    CHECK(mv_key_note(&g, 1, 0) == 51);     /* black above F: D3 + 1 */
    CHECK(mv_key_note(&g, 26, 3) == -1 || mv_key_note(&g, 26, 3) <= 127);

    /* Just intonation: the major third from the root is ~14 cents flat. */
    fresh();
    glob.tuning = TU_JUST;
    glob.root = 9;                          /* A */
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 61, 100);                   /* C#4, a third above A */
    render(22050);
    double cents = 1200 * log2(pitch(4096, 4096) / hz(61));
    CHECK(fabs(cents + 13.7) < 4);
}

static void write_wav(void)
{
    /* A phrase in hirajoshi on the white keys, with presses and a chord. */
    static int16_t pcm[2 * 44100 * 12];
    unsigned at = 0;
    mv_init(&e);
    mv_patch_default(&patch, MV_KOTO);
    mv_global_default(&glob);
    glob.map = MAP_WHITE;
    mv_prepare(&e, &patch, &glob);
    static const int8_t keys[] = { 0, 2, 4, 6, 7, 9, 7, 6, -1, 4, -2, 2, 0, 9, 11, 12 };
    for (unsigned i = 0; i < sizeof keys; i++) {
        int k = keys[i];
        if (k == -1) e.pressing = 1;
        else if (k == -2) e.pressing = 0;
        else {
            int n = mv_key_note(&glob, (unsigned)k, 0);
            mv_note(&e, (unsigned)n, 100);
        }
        render(k < 0 ? 11025 : 9216);
        unsigned len = k < 0 ? 11025 : 9216;
        for (unsigned f = 0; f < len && at < 44100 * 12; f++, at++) {
            pcm[2 * at] = (int16_t)(buf[2 * f] >> 8);
            pcm[2 * at + 1] = (int16_t)(buf[2 * f + 1] >> 8);
        }
    }
    render(44100 * 2);
    for (unsigned f = 0; f < 44100 * 2 && at < 44100 * 12; f++, at++) {
        pcm[2 * at] = (int16_t)(buf[2 * f] >> 8);
        pcm[2 * at + 1] = (int16_t)(buf[2 * f + 1] >> 8);
    }
    FILE *w = fopen("build-host/mavis_koto.wav", "wb");
    if (!w) return;
    uint32_t bytes = at * 4, rate = 44100, brate = rate * 4, chunk = 36 + bytes, fmt = 16;
    uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, w); fwrite(&chunk, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
    fwrite(&fmt, 4, 1, w); fwrite(&pcmfmt, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w);
    fwrite(&brate, 4, 1, w); fwrite(&align, 2, 1, w); fwrite(&bits, 2, 1, w);
    fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(pcm, 4, at, w);
    fclose(w);
    printf("  koto: build-host/mavis_koto.wav (%.1f s)\n", at / 44100.0);
}

int main(void)
{
    test_tuning();
    test_life();
    test_bends();
    test_keys_and_tunings();
    write_wav();
    if (failures) { printf("test_mavis_koto: %d failure(s)\n", failures); return 1; }
    printf("test_mavis_koto: ok\n");
    return 0;
}
