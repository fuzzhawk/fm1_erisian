/* deps: app/engine/synth.c app/engine/tables.c */
/* Keys engines: every engine sounds, is in tune, ends after release; voice
 * pool, mono/legato note stack, zones and octaves. Writes build-host/keys.wav
 * (each engine playing a short phrase). */
#include "../app/engine/synth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct synth s;
static struct synth_patch patch;
#define MAXF (44100 * 4)
static int32_t buf[2 * MAXF];

static void render(unsigned frames)
{
    memset(buf, 0, sizeof buf);
    for (unsigned at = 0; at + 128 <= frames; at += 128) synth_render(&s, buf + 2 * at, 128);
}

static double peak(unsigned from, unsigned to)
{
    double p = 0;
    for (unsigned f = from; f < to; f++) { double a = fabs(buf[2 * f] / 8388608.0); if (a > p) p = a; }
    return p;
}

/* Fundamental by autocorrelation over [from, from + n), lags for 40..2000 Hz.
 * Picks the first lag whose correlation is close to the best (avoids
 * octave errors on harmonics). */
static double pitch(unsigned from, unsigned n)
{
    static double c[1200];
    double best = 0;
    for (unsigned lag = 22; lag < 1103; lag++) {
        double acc = 0, e0 = 0, e1 = 0;
        for (unsigned i = 0; i < n; i++) {
            double a = buf[2 * (from + i)], b = buf[2 * (from + i + lag)];
            acc += a * b; e0 += a * a; e1 += b * b;
        }
        c[lag] = e0 > 0 && e1 > 0 ? acc / sqrt(e0 * e1) : 0;
        if (c[lag] > best) best = c[lag];
    }
    for (unsigned lag = 23; lag < 1102; lag++)
        if (c[lag] > 0.9 * best && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
            /* parabolic refinement */
            double a = c[lag - 1], b = c[lag], d = c[lag + 1];
            double off = (a - d) / (2 * (a - 2 * b + d));
            return 44100.0 / (lag + (fabs(off) < 1 ? off : 0));
        }
    return 0;
}

static void use(unsigned engine)
{
    synth_init(&s);
    synth_patch_default(&patch);
    synth_layer_engine_defaults(&patch.layer[0], engine);
    synth_prepare(&s, &patch);
}

static unsigned active_voices(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SYNTH_VOICES; i++) n += s.voice[i].active;
    return n;
}

static void test_engines(void)
{
    /* Each engine in tune at A3 (220 Hz) and A4 (440 Hz). Engines with noise
     * or pitch sweeps get their clean settings for the pitch check. */
    for (unsigned e = SY_BASS; e < SY_ENGINES; e++) {
        for (unsigned k = 0; k < 2; k++) {
            unsigned note = k ? 69 : 57;
            double want = k ? 440.0 : 220.0;
            use(e);
            if (e == SY_BASS) { patch.layer[0].e[BA_PENV] = 0; patch.layer[0].e[BA_CLICK] = 0; }
            if (e == SY_LEAD) patch.layer[0].e[LE_VDEPTH] = 0;
            if (e == SY_FLUTE) { patch.layer[0].e[FL_VDEPTH] = 0; patch.layer[0].e[FL_BREATH] = 0;
                                 patch.layer[0].e[FL_AIR] = 0; patch.layer[0].e[FL_CHIFF] = 0; }
            if (e == SY_VIBES) patch.layer[0].e[VI_TDEPTH] = 0;
            if (e == SY_PAD) { patch.layer[0].e[PA_RESO] = 127; patch.layer[0].e[PA_AIR] = 0;
                               patch.layer[0].e[PA_BRIGHT] = 0; patch.layer[0].c[SC_ATTACK] = 0; }
            synth_prepare(&s, &patch);
            synth_note(&s, note, 110);
            render(44100);
            double p = peak(0, 44100);
            double f = pitch(e == SY_PAD ? 22050 : 6000, 4096);
            if (fabs(f - want) / want > 0.01 || p < 0.05 || p > 1.0)
                fprintf(stderr, "  %s note %u: %.1f Hz (want %.1f), peak %.3f\n",
                        synth_engine_name(e), note, f, want, p);
            CHECK(p > 0.05 && p <= 1.0);
            CHECK(fabs(f - want) / want < (e == SY_KOTO || e == SY_PAD ? 0.02 : 0.01));
        }
    }
}

static void test_release_and_pool(void)
{
    /* Every engine goes quiet and frees its voice after note off. */
    for (unsigned e = SY_BASS; e < SY_ENGINES; e++) {
        use(e);
        synth_note(&s, 60, 100);
        render(4410);
        synth_note(&s, 60, 0);
        render(44100 * 4);
        CHECK(active_voices() == 0);
        CHECK(peak(44100 * 3, 44100 * 4 - 128) < 0.001);
    }

    /* Poly: more notes than voices steals; the newest notes sound. */
    use(SY_LEAD);
    for (unsigned i = 0; i < SYNTH_VOICES + 2; i++) synth_note(&s, 50 + i, 100);
    CHECK(active_voices() == SYNTH_VOICES);
    unsigned newest = 0;
    for (unsigned i = 0; i < SYNTH_VOICES; i++) newest += s.voice[i].key >= 52;
    CHECK(newest == SYNTH_VOICES);
    /* A released voice is reused before a held one. */
    synth_note(&s, 55, 0);
    synth_note(&s, 80, 100);
    int held_55 = 0, has_80 = 0;
    for (unsigned i = 0; i < SYNTH_VOICES; i++) {
        if (s.voice[i].key == 55) held_55 = 1;
        if (s.voice[i].key == 80) has_80 = 1;
    }
    CHECK(has_80 && !held_55);
    synth_all_off(&s);
    render(44100 * 3);
    CHECK(active_voices() == 0);
}

static void test_mono_legato(void)
{
    /* 808 (legato + glide): overlapping notes glide on one voice, no
     * retrigger; releasing the top note glides back to the held one. */
    use(SY_BASS);
    patch.layer[0].e[BA_PENV] = 0;
    patch.layer[0].c[SC_SUSTAIN] = 127;
    synth_prepare(&s, &patch);
    synth_note(&s, 45, 100);
    render(8192);
    synth_note(&s, 57, 100);
    CHECK(active_voices() == 1);
    render(22050);
    CHECK(fabs(pitch(16000, 4096) - 220.0) < 3.0);       /* glided up an octave */
    synth_note(&s, 57, 0);
    render(22050);
    CHECK(active_voices() == 1);
    CHECK(fabs(pitch(16000, 4096) - 110.0) < 2.0);       /* back to the held A2 */
    synth_note(&s, 45, 0);
    render(44100);
    CHECK(active_voices() == 0);

    /* Glide really takes time: halfway through it is between the notes. */
    use(SY_BASS);
    patch.layer[0].e[BA_PENV] = 0;
    patch.layer[0].c[SC_SUSTAIN] = 127;
    patch.layer[0].c[SC_GLIDE] = 70;
    synth_prepare(&s, &patch);
    synth_note(&s, 45, 100);
    render(4096);
    synth_note(&s, 69, 100);
    uint32_t start = s.voice[0].inc;
    render(512);
    CHECK(s.voice[0].inc > start && s.voice[0].inc < s.voice[0].target);

    /* MONO retriggers the envelope; LEGATO does not. */
    use(SY_LEAD);
    patch.layer[0].mode = SM_MONO;
    synth_prepare(&s, &patch);
    synth_note(&s, 60, 100);
    render(8192);
    synth_note(&s, 64, 100);
    CHECK(active_voices() == 1 && s.voice[0].stage == 0);
    patch.layer[0].mode = SM_LEGATO;
    synth_prepare(&s, &patch);
    render(8192);
    synth_note(&s, 67, 100);
    CHECK(active_voices() == 1 && s.voice[0].stage != 0 && s.voice[0].key == 67);
}

static void test_layers(void)
{
    /* Split: layer 1 below C4, layer 2 from C4; plus a stacked octave. */
    synth_init(&s);
    synth_patch_default(&patch);
    synth_layer_engine_defaults(&patch.layer[0], SY_BASS);
    patch.layer[0].hi = 59;
    synth_layer_engine_defaults(&patch.layer[1], SY_LEAD);
    patch.layer[1].lo = 60;
    synth_prepare(&s, &patch);
    synth_note(&s, 48, 100);
    synth_note(&s, 64, 100);
    unsigned l0 = 0, l1 = 0;
    for (unsigned i = 0; i < SYNTH_VOICES; i++)
        if (s.voice[i].active) { l0 += s.voice[i].layer == 0; l1 += s.voice[i].layer == 1; }
    CHECK(l0 == 1 && l1 == 1);

    /* Stack: two layers on every key, one an octave up. */
    synth_init(&s);
    synth_patch_default(&patch);
    synth_layer_engine_defaults(&patch.layer[0], SY_LEAD);
    patch.layer[0].e[LE_VDEPTH] = 0;
    patch.layer[0].c[SC_LEVEL] = 0;            /* listen to layer 2 only */
    synth_layer_engine_defaults(&patch.layer[1], SY_LEAD);
    patch.layer[1].e[LE_VDEPTH] = 0;
    patch.layer[1].e[LE_MOD] = 0;
    patch.layer[1].octave = 1;
    synth_prepare(&s, &patch);
    synth_note(&s, 57, 100);
    CHECK(active_voices() == 2);
    render(22050);
    CHECK(fabs(pitch(8000, 4096) - 440.0) < 4.0);

    /* A layer switched off while sounding goes quiet. */
    patch.layer[1].engine = SY_OFF;
    synth_prepare(&s, &patch);
    render(1024);
    CHECK(peak(256, 1024) < 0.0001);
}

static void write_wav(void)
{
    /* One phrase per engine: a short arpeggio, released. */
    static int16_t pcm[2 * 44100 * 2 * 6];
    unsigned at = 0;
    static const uint8_t notes[4] = { 57, 60, 64, 69 };
    for (unsigned e = SY_BASS; e < SY_ENGINES; e++) {
        use(e);
        for (unsigned n = 0; n < 4; n++) {
            unsigned note = e == SY_BASS ? notes[n] - 24 : notes[n];
            synth_note(&s, note, 110);
            render(8192);
            synth_note(&s, note, 0);
            for (unsigned f = 0; f < 8192 && at < 44100 * 12; f++, at++) {
                pcm[2 * at] = (int16_t)(buf[2 * f] >> 8);
                pcm[2 * at + 1] = (int16_t)(buf[2 * f + 1] >> 8);
            }
        }
        render(16384);
        for (unsigned f = 0; f < 16384 && at < 44100 * 12; f++, at++) {
            pcm[2 * at] = (int16_t)(buf[2 * f] >> 8);
            pcm[2 * at + 1] = (int16_t)(buf[2 * f + 1] >> 8);
        }
    }
    FILE *w = fopen("build-host/keys.wav", "wb");
    if (!w) return;
    uint32_t bytes = at * 4, rate = 44100, brate = rate * 4, chunk = 36 + bytes, fmt = 16;
    uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, w); fwrite(&chunk, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
    fwrite(&fmt, 4, 1, w); fwrite(&pcmfmt, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w);
    fwrite(&brate, 4, 1, w); fwrite(&align, 2, 1, w); fwrite(&bits, 2, 1, w);
    fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(pcm, 4, at, w);
    fclose(w);
    printf("  keys: build-host/keys.wav (%.1f s)\n", at / 44100.0);
}

int main(void)
{
    test_engines();
    test_release_and_pool();
    test_mono_legato();
    test_layers();
    write_wav();
    if (failures) { printf("test_synth: %d failure(s)\n", failures); return 1; }
    printf("test_synth: ok\n");
    return 0;
}
