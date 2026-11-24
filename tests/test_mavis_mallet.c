/* deps: mavis/engine/engine.c mavis/engine/mtables.c */
/* MAVIS MALLET: every material has its fundamental in tune, materials decay
 * differently, the damper works, unison doubles the strings, stretch spreads
 * partials, the resonator budget caps detail. Writes build-host/mavis_mallet.wav. */
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
#define MAXF (44100 * 4)
static int32_t buf[2 * MAXF];

static void render(unsigned frames)
{
    memset(buf, 0, sizeof buf);
    for (unsigned at = 0; at + 128 <= frames; at += 128) mv_render(&e, buf + 2 * at, 128);
}

static double mono(unsigned f) { return (buf[2 * f] + buf[2 * f + 1]) / 2 / 8388608.0; }

static double rms(unsigned from, unsigned to)
{
    double s = 0;
    for (unsigned f = from; f < to; f++) s += mono(f) * mono(f);
    return sqrt(s / (to - from));
}

/* Magnitude at freq over [from, from + n) (Goertzel with a Hann window). */
static double mag(double freq, unsigned from, unsigned n)
{
    double w = 2 * M_PI * freq / 44100, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (unsigned i = 0; i < n; i++) {
        double x = mono(from + i) * (0.5 - 0.5 * cos(2 * M_PI * i / n));
        double s0 = x + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

/* The strongest frequency within +-60 cents of want, in cents from want. */
static double cents_off(double want, unsigned from, unsigned n)
{
    double best = 0, at = 0;
    for (int c = -60; c <= 60; c++) {
        double m = mag(want * pow(2, c / 1200.0), from, n);
        if (m > best) { best = m; at = c; }
    }
    return at;
}

static void use(unsigned material)
{
    mv_init(&e);
    mv_patch_default(&patch, MV_MALLET);
    mv_global_default(&glob);
    patch.p[MA_HUMAN] = 0;
    patch.p[MA_MATERIAL] = (uint8_t)material;
    mv_prepare(&e, &patch, &glob);
}

static unsigned active(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MV_MAX_VOICES; i++) n += e.voice[i].active;
    return n;
}

int main(void)
{
    static const unsigned materials[] = { 0, 25, 51, 76, 102, 127 };
    static const char *const names[] = { "kalimba", "glock", "xylo", "vibes", "marimba", "string" };

    /* In tune at A3 and A4, and actually sounding. */
    for (unsigned i = 0; i < 6; i++)
        for (unsigned k = 0; k < 2; k++) {
            unsigned note = k ? 69 : 57;
            double want = k ? 440.0 : 220.0;
            use(materials[i]);
            mv_note(&e, note, 100);
            render(22050);
            double off = cents_off(want, 1024, 16384);
            double level = rms(0, 8192);
            if (fabs(off) > 3 || level < 0.003)
                fprintf(stderr, "  %s %u: %+.0f cents, rms %.4f\n", names[i], note, off, level);
            CHECK(fabs(off) <= 3);
            CHECK(level > 0.003);
        }

    /* Vibes ring much longer than xylophone. */
    use(76);
    mv_note(&e, 69, 100);
    render(44100 * 2);
    double vibes = rms(44100, 44100 + 8192) / rms(0, 8192);
    use(51);
    mv_note(&e, 69, 100);
    render(44100 * 2);
    double xylo = rms(44100, 44100 + 8192) / rms(0, 8192);
    CHECK(vibes > 4 * xylo);

    /* The damper: a short release goes quiet fast; 127 lets it ring. */
    use(76);
    patch.p[MA_RELEASE] = 20;
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 69, 100);
    render(4410);
    mv_note(&e, 69, 0);
    render(44100);
    CHECK(rms(22050, 30000) < 0.0003);
    use(76);
    patch.p[MA_RELEASE] = 127;
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 69, 100);
    render(4410);
    mv_note(&e, 69, 0);
    render(44100);
    CHECK(rms(22050, 30000) > 0.002);

    /* Unison doubles the resonators; stretch raises the 10th partial. */
    use(127);
    patch.p[MA_UNISON] = 60;
    mv_prepare(&e, &patch, &glob);
    mv_note(&e, 45, 100);
    CHECK(e.voice[0].nres >= 2 * MV_MODES - 2);
    use(127);
    patch.p[MA_STRETCH] = 0;
    mv_prepare(&e, &patch, &glob);
    uint32_t straight = e.mallet.ratio_q16[9];
    patch.p[MA_STRETCH] = 127;
    mv_prepare(&e, &patch, &glob);
    CHECK(abs((int)straight - 10 * 65536) < 100);
    CHECK(e.mallet.ratio_q16[9] > straight + 6000);

    /* Budget: with many voices sounding, each gets fewer modes. */
    use(102);
    e.res_budget = 48;
    for (unsigned i = 0; i < 8 && i < glob.voices; i++) mv_note(&e, 50 + 3 * i, 100);
    unsigned last = 0;
    for (unsigned i = 0; i < MV_MAX_VOICES; i++) if (e.voice[i].active) last = e.voice[i].nres;
    CHECK(last <= 48u / glob.voices + 1u);
    render(44100 * 4);
    CHECK(active() == 0);                          /* all rang out */

    /* The governor: heavy load shrinks the budget, light load grows it. */
    use(102);
    uint32_t b0 = e.res_budget;
    e.load_q16 = 60000;
    render(128 * 10);
    CHECK(e.res_budget < b0);
    e.load_q16 = 1000;
    render(128 * 40);
    CHECK(e.res_budget > b0 - 20);

    /* A demo: marimba run, vibes with motor, a piano-ish chord. */
    static int16_t pcm[2 * 44100 * 14];
    unsigned at = 0;
    static const struct { uint8_t material, trem, unison; } demo[3] = { { 102, 0, 0 }, { 76, 60, 0 }, { 127, 0, 40 } };
    static const uint8_t run[] = { 57, 60, 62, 64, 67, 69, 72, 69 };
    for (unsigned d = 0; d < 3; d++) {
        use(demo[d].material);
        patch.p[MA_TREM] = demo[d].trem;
        patch.p[MA_UNISON] = demo[d].unison;
        patch.p[MA_HUMAN] = 25;
        if (d == 1) patch.p[MA_RELEASE] = 127;
        mv_prepare(&e, &patch, &glob);
        for (unsigned i = 0; i < 8; i++) {
            if (d == 2 && i < 4) { mv_note(&e, run[i], 100); continue; }   /* chord */
            if (d != 2) mv_note(&e, run[i], 100);
            render(6615);
            for (unsigned f = 0; f < 6615 && at < 44100 * 14; f++, at++) {
                pcm[2 * at] = (int16_t)(buf[2 * f] >> 8);
                pcm[2 * at + 1] = (int16_t)(buf[2 * f + 1] >> 8);
            }
            if (d != 2) mv_note(&e, run[i], 0);
        }
        render(22050);
        for (unsigned f = 0; f < 22050 && at < 44100 * 14; f++, at++) {
            pcm[2 * at] = (int16_t)(buf[2 * f] >> 8);
            pcm[2 * at + 1] = (int16_t)(buf[2 * f + 1] >> 8);
        }
    }
    FILE *w = fopen("build-host/mavis_mallet.wav", "wb");
    if (w) {
        uint32_t bytes = at * 4, rate = 44100, brate = rate * 4, chunk = 36 + bytes, fmt = 16;
        uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
        fwrite("RIFF", 1, 4, w); fwrite(&chunk, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w);
        fwrite(&fmt, 4, 1, w); fwrite(&pcmfmt, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w);
        fwrite(&brate, 4, 1, w); fwrite(&align, 2, 1, w); fwrite(&bits, 2, 1, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(pcm, 4, at, w);
        fclose(w);
        printf("  mallet: build-host/mavis_mallet.wav (%.1f s)\n", at / 44100.0);
    }

    if (failures) { printf("test_mavis_mallet: %d failure(s)\n", failures); return 1; }
    printf("test_mavis_mallet: ok\n");
    return 0;
}
