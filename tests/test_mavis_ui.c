/* deps: mavis/ui/mavis_ui.c shared/ui/draw.c shared/ui/font5x7.c mavis/engine/engine.c mavis/engine/mtables.c */
/* MAVIS UI: keys play (octave-safe note-offs), the KEYS page changes the
 * map, knobs edit what is on screen, LFO presses, the visualiser draws a
 * sounding string. Writes build-host/mavis_*.ppm. */
#include "../mavis/mavis.h"
#include "../shared/ui/draw.h"
#include "fm1.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct mavis_engine engine;
struct mv_patch patch;
struct mv_patch bank[MV_ENGINES];
struct mv_global global;

static unsigned notes[64][2], nnotes;
static int pressed, saves;
void audio_note(unsigned n, unsigned v) { if (nnotes < 64) { notes[nnotes][0] = n; notes[nnotes][1] = v; nnotes++; } }
void audio_all_off(void) {}
void audio_press(int on) { pressed = on; }
void audio_set_master(uint16_t k) { (void)k; }
int audio_sounding(void) { return 0; }
unsigned audio_voices(void) { return 0; }
unsigned audio_load(void) { return 12; }
unsigned audio_load_peak(int r) { (void)r; return 20; }
int sound_save(void) { saves++; return 0; }
const char *status_text(void) { return "CPU 12%  VOICES 0/6"; }

static uint16_t fb[FM1_SCREEN_HEIGHT][FM1_SCREEN_WIDTH];
static uint32_t millis;
static struct fm1_controls next;
void fm1_screen_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c)
{
    CHECK(x + w <= FM1_SCREEN_WIDTH && y + h <= FM1_SCREEN_HEIGHT);
    for (unsigned j = y; j < y + h && j < FM1_SCREEN_HEIGHT; j++)
        for (unsigned i = x; i < x + w && i < FM1_SCREEN_WIDTH; i++) fb[j][i] = c;
}
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *px, size_t n)
{
    CHECK(x + w <= FM1_SCREEN_WIDTH && y + h <= FM1_SCREEN_HEIGHT && n == (size_t)w * h);
    for (unsigned j = 0; j < h; j++)
        for (unsigned i = 0; i < w; i++) fb[y + j][x + i] = px[j * w + i];
    return 0;
}
void fm1_controls_read(struct fm1_controls *c) { *c = next; memset(next.encoders, 0, sizeof next.encoders); }
void fm1_led_set(unsigned key, int on) { (void)key; (void)on; }
uint32_t fm1_millis(void) { return millis; }

static void scan(void) { millis += 100; ui_poll(millis); millis += 100; ui_poll(millis); }
static void press(unsigned b) { next.buttons |= 1u << b; scan(); next.buttons &= ~(1u << b); scan(); }
static void turn(unsigned e, int d) { next.encoders[e] = (int16_t)d; scan(); }
static void key(unsigned k) { next.keys = 1u << k; scan(); next.keys = 0; scan(); }

static void screenshot(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "build-host/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT);
    for (unsigned y = 0; y < FM1_SCREEN_HEIGHT; y++)
        for (unsigned x = 0; x < FM1_SCREEN_WIDTH; x++) {
            uint16_t c = fb[y][x];
            uint8_t rgb[3] = { (uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

int main(void)
{
    mv_init(&engine);
    for (unsigned e = 0; e < MV_ENGINES; e++) mv_patch_default(&bank[e], e);
    mv_patch_default(&patch, MV_KOTO);
    mv_global_default(&global);
    mv_prepare(&engine, &patch, &global);
    ui_init();
    ui_redraw();

    key(0);
    CHECK(nnotes == 2 && notes[0][0] == 53 && notes[0][1] && notes[1][0] == 53 && !notes[1][1]);

    /* Octave up while held: the note-off still matches. */
    nnotes = 0;
    next.keys = 1; scan();
    press(1);                                  /* OCT+ */
    next.keys = 0; scan();
    CHECK(nnotes == 2 && notes[1][0] == 53 && notes[1][1] == 0);
    press(0);                                  /* OCT- */

    /* Quick strip on PLAY: KNOB2 = BRITE. */
    unsigned b = patch.p[KT_BRIGHT];
    turn(3, 1);
    CHECK(patch.p[KT_BRIGHT] == b + 1);

    /* LFO held presses the strings. */
    next.buttons = 1u << 5; scan();
    CHECK(pressed == 1);
    next.buttons = 0; scan();
    CHECK(pressed == 0);

    /* The visualiser draws the newest string. */
    mv_note(&engine, 57, 100);
    static int32_t out[256];
    for (unsigned i = 0; i < 20; i++) { memset(out, 0, sizeof out); mv_render(&engine, out, 128); }
    millis += 100;
    ui_visual(millis);
    unsigned lit = 0;
    for (unsigned y = 46; y < 142; y++) for (unsigned x = 0; x < 240; x++) lit += fb[y][x] == RGB565(255, 92, 56);
    CHECK(lit > 300);
    screenshot("mavis_play");

    /* SOUND pages: EDIT, then ALGORITHM; KNOB1 on PLUCK = POS. */
    press(6);
    turn(1, 1);
    unsigned pos = patch.p[KT_POS];
    turn(2, 1);
    CHECK(patch.p[KT_POS] == pos + 1);
    screenshot("mavis_sound");

    /* KEYS: map to WHITE; the first key plays the root (D3). */
    press(6);
    turn(4, 1);                                /* KNOB3 = MAP */
    CHECK(global.map == MAP_WHITE);
    screenshot("mavis_keys");
    nnotes = 0;
    key(0);
    CHECK(nnotes == 2 && notes[0][0] == 50);
    /* ROOT down a semitone: C#3. */
    turn(5, -1);
    nnotes = 0;
    key(0);
    CHECK(notes[0][0] == 49);

    press(9);                                  /* SAVE */
    CHECK(saves == 1);

    /* PRESETS: to MALLET; KOTO's edits are kept in the bank. */
    press(6);                                  /* EDIT: back to PLAY */
    unsigned koto_bright = patch.p[KT_BRIGHT];
    turn(6, 1);
    CHECK(patch.engine == MV_MALLET && engine.engine == MV_MALLET);
    mv_note(&engine, 57, 100);
    for (unsigned i = 0; i < 10; i++) { memset(out, 0, sizeof out); mv_render(&engine, out, 128); }
    millis += 100;
    ui_visual(millis);
    lit = 0;
    for (unsigned y = 46; y < 142; y++) for (unsigned x = 0; x < 240; x++) lit += fb[y][x] == RGB565(255, 176, 40);
    CHECK(lit > 300);                          /* mode bars */
    screenshot("mavis_mallet_play");
    press(6);                                  /* SOUND: MATERIAL page */
    unsigned mat = patch.p[MA_MATERIAL];
    turn(2, -1);
    CHECK(patch.p[MA_MATERIAL] == mat - 1);
    turn(1, 1);                                /* MALLET page: KNOB1 = HARD */
    unsigned hard = patch.p[MA_HARD];
    turn(2, 1);
    CHECK(patch.p[MA_HARD] == hard + 1);
    screenshot("mavis_mallet_sound");
    turn(6, -1);                               /* back to KOTO */
    CHECK(patch.engine == MV_KOTO && patch.p[KT_BRIGHT] == koto_bright);
    CHECK(bank[MV_MALLET].p[MA_HARD] == hard + 1);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("mavis ui ok\n");
    return 0;
}
