/* deps: app/ui/keys_ui.c app/ui/draw.c app/ui/font5x7.c app/engine/synth.c app/engine/drums.c app/engine/seq.c app/engine/tables.c */
/* Keys UI: keys play notes (octave-safe note-offs), SEL/SELECT pick the
 * layer, EDIT changes mode, ALGORITHM pages, knobs edit what is on screen,
 * SEQ asks to switch. Writes build-host/keys_*.ppm screenshots. */
#include "../app/erisian.h"
#include "../app/ui/draw.h"
#include "fm1.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct seq_pattern pattern;
struct drum_kit kit;
struct drums drums;
struct seq_state seq;
struct synth_patch patch;
struct synth synth;

static int playing, keys_off;
static unsigned notes[64][2], nnotes;
void audio_play(void) { playing = 1; }
void audio_stop(void) { playing = 0; }
int audio_playing(void) { return playing; }
void audio_set_master(uint16_t knob) { (void)knob; }
void audio_note(unsigned key, unsigned velocity)
{
    if (nnotes < 64) { notes[nnotes][0] = key; notes[nnotes][1] = velocity; nnotes++; }
}
void audio_keys_off(void) { keys_off++; }
int project_save(int now) { (void)now; return 0; }
const char *status_text(void) { return "TEST"; }

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

static int last_result;
static void scan(void)
{
    millis += 100;
    last_result = keys_ui_poll(millis);
    millis += 100;
    int r = keys_ui_poll(millis);
    if (!last_result) last_result = r;
}
static int press(unsigned button)
{
    next.buttons |= 1u << button;
    scan();
    int r = last_result;
    next.buttons &= ~(1u << button);
    scan();
    return r;
}
static void turn(unsigned encoder, int detents) { next.encoders[encoder] = (int16_t)detents; scan(); }

enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_SEL = 3, BTN_EDIT = 6, BTN_HOME = 8, BTN_SEQ = 11 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_K1 = 2, ENC_K2 = 3, ENC_K3 = 4, ENC_K4 = 5 };

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
    seq_pattern_init(&pattern);
    drum_kit_default(&kit);
    drums_init(&drums);
    synth_init(&synth);
    synth_patch_default(&patch);
    synth_prepare(&synth, &patch);
    keys_ui_init();
    keys_ui_redraw();
    screenshot("keys_play");

    /* Key 0 = F3 (MIDI 53); off on release. */
    next.keys = 1u << 0; scan();
    next.keys = 0; scan();
    CHECK(nnotes == 2 && notes[0][0] == 53 && notes[0][1] > 0 && notes[1][0] == 53 && notes[1][1] == 0);
    /* The top key is G5 (MIDI 79). */
    nnotes = 0;
    next.keys = 1u << 26; scan(); next.keys = 0; scan();
    CHECK(nnotes == 2 && notes[0][0] == 79);

    /* Octave up while a key is held: its note-off still matches. */
    nnotes = 0;
    next.keys = 1u << 0; scan();
    press(BTN_OCT_UP);
    next.keys = 0; scan();
    next.keys = 1u << 0; scan(); next.keys = 0; scan();
    CHECK(nnotes == 4 && notes[1][0] == 53 && notes[1][1] == 0 && notes[2][0] == 65 && notes[3][0] == 65);
    press(BTN_OCT_DOWN);

    /* PLAY quick strip edits layer 1: KNOB2 = RESO. */
    unsigned reso = patch.layer[0].c[SC_RESO];
    turn(ENC_K2, 1);
    CHECK(patch.layer[0].c[SC_RESO] == reso + 1);

    /* SEL -> layer 2 (OFF). EDIT -> ENGINE: KNOB1 picks its engine. */
    press(BTN_SEL);
    press(BTN_EDIT);
    keys_off = 0;
    turn(ENC_K1, 1);
    CHECK(patch.layer[1].engine == SY_BASS && keys_off == 1);
    CHECK(synth.derived[1].engine == SY_BASS);             /* prepared */
    screenshot("keys_engine");
    /* ALGORITHM -> ENGINE 2: KNOB1 = DRIVE (e3). */
    turn(ENC_ALGORITHM, 1);
    unsigned drive = patch.layer[1].e[BA_DRIVE];
    turn(ENC_K1, 1);
    CHECK(patch.layer[1].e[BA_DRIVE] == drive + 1);
    /* SELECT back to layer 1 (FM LEAD): page 2 still has parameters. */
    turn(ENC_SELECT, -1);
    unsigned msus = patch.layer[0].e[LE_MSUS];
    turn(ENC_K2, 1);
    CHECK(patch.layer[0].e[LE_MSUS] == msus + 1);
    /* FM RATIO steps one ratio per detent. */
    turn(ENC_ALGORITHM, -1);
    unsigned ratio = patch.layer[0].e[LE_RATIO] * 16u / 128u;
    turn(ENC_K3, 1);
    CHECK(patch.layer[0].e[LE_RATIO] * 16u / 128u == ratio + 1);

    /* EDIT -> SHAPE (AMP): KNOB1 = attack. */
    press(BTN_EDIT);
    unsigned atk = patch.layer[0].c[SC_ATTACK];
    turn(ENC_K1, 3);                                   /* fast: +6 */
    CHECK(patch.layer[0].c[SC_ATTACK] == atk + 6);
    screenshot("keys_amp");

    /* EDIT -> LAYER: MODE, LOW, HIGH, OCT; a split keeps lo <= hi. */
    press(BTN_EDIT);
    turn(ENC_K1, 1);
    CHECK(patch.layer[0].mode == SM_MONO);
    turn(ENC_K3, -60);                                 /* hi down to 7 */
    CHECK(patch.layer[0].hi == 7);
    turn(ENC_K2, 60);                                  /* lo can't pass hi */
    CHECK(patch.layer[0].lo == 7);
    turn(ENC_K4, 1);
    CHECK(patch.layer[0].octave == 1 && synth.derived[0].transpose == 12);
    screenshot("keys_layer");

    /* EDIT wraps to PLAY; SEQ asks main.c to switch, HOME for status. */
    press(BTN_EDIT);
    CHECK(press(BTN_SEQ) == UI_SWITCH);
    CHECK(press(BTN_HOME) == UI_HOME);
    screenshot("keys_play2");

    /* Leaving releases everything. */
    keys_off = 0;
    keys_ui_leave();
    CHECK(keys_off == 1);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("keys ui ok\n");
    return 0;
}
