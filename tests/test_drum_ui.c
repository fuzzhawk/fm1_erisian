/* deps: app/ui/drum_ui.c app/ui/draw.c app/ui/font5x7.c app/engine/drums.c app/engine/seq.c app/engine/tables.c */
/* Drum UI tests: drive the panel with scripted controls against a fake
 * screen. Checks that EDIT changes mode, ALGORITHM changes page, the grid is
 * hidden while editing, and the knobs edit what the screen shows. Writes
 * build-host/ui_*.ppm screenshots. */
#include "../app/erisian.h"
#include "../app/ui/draw.h"
#include "fm1.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* --- the app state the UI edits ----------------------------------------- */
struct seq_pattern pattern;
struct drum_kit kit;
struct drums drums;
struct seq_state seq;

static int playing;
void audio_play(void) { playing = 1; }
void audio_stop(void) { playing = 0; }
int audio_playing(void) { return playing; }
void audio_pad(unsigned v, unsigned vel) { (void)v; (void)vel; }
void audio_set_master(uint16_t knob) { (void)knob; }
int project_save(int now) { (void)now; return 0; }
const char *status_text(void) { return "TEST"; }

/* --- fake panel ---------------------------------------------------------- */
static uint16_t fb[FM1_SCREEN_HEIGHT][FM1_SCREEN_WIDTH];
static unsigned cell_fills;   /* fills the size of a grid cell */
static uint32_t millis;
static struct fm1_controls next;

void fm1_screen_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t c)
{
    CHECK(x + w <= FM1_SCREEN_WIDTH && y + h <= FM1_SCREEN_HEIGHT);
    if (w == 11 && h == 10) cell_fills++;
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

/* One panel scan with these controls held (encoders turned this scan). */
static void scan(void)
{
    millis += 100;
    drum_ui_poll(millis);
    millis += 100;
    drum_ui_poll(millis);      /* releases nothing; lets the strip redraw */
}
static void press(unsigned button)
{
    next.buttons |= 1u << button;
    scan();
    next.buttons &= ~(1u << button);
    scan();
}
static void turn(unsigned encoder, int detents)
{
    next.encoders[encoder] = (int16_t)detents;
    scan();
}

enum { BTN_EDIT = 6, ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_K1 = 2, ENC_K2 = 3, ENC_K4 = 5 };

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
    drums_prepare(&drums, &kit);

    drum_ui_init();
    CHECK(cell_fills >= SEQ_VOICES * 16);          /* SEQ view: the grid */
    screenshot("ui_seq");

    /* SEQ quick strip: KNOB1 = TUNE, KNOB4 = LEVEL of the selected voice. */
    unsigned tune = kit.voice[0].e[E_TUNE];
    turn(ENC_K1, 1);
    CHECK(kit.voice[0].e[E_TUNE] == tune + 1);
    unsigned level = kit.voice[0].c[DC_LEVEL];
    turn(ENC_K4, -1);
    CHECK(kit.voice[0].c[DC_LEVEL] == level - 1);

    /* SELECT to SD (FM, four engine pages' worth of parameters). */
    turn(ENC_SELECT, 1);

    /* EDIT -> ENGINE mode: full screen, no grid drawing. */
    press(BTN_EDIT);
    cell_fills = 0;
    unsigned engine = kit.voice[1].engine;
    CHECK(engine == DRUM_FM);
    screenshot("ui_engine");
    /* KNOB2 on the first engine page is FM TUNE. */
    unsigned fm_tune = kit.voice[1].e[FM_TUNE];
    turn(ENC_K2, 1);
    CHECK(kit.voice[1].e[FM_TUNE] == fm_tune + 1);
    /* ALGORITHM -> ENGINE 2: KNOB1 is engine parameter 3 (P.DEC). */
    turn(ENC_ALGORITHM, 1);
    unsigned pdec = kit.voice[1].e[FM_PDEC];
    turn(ENC_K1, 1);
    CHECK(kit.voice[1].e[FM_PDEC] == pdec + 1);
    screenshot("ui_engine2");
    /* ALGORITHM back to page 1 and once more backwards wraps to the last
     * FM page (parameters 7..10): KNOB2 is FM NOISE. */
    turn(ENC_ALGORITHM, -1);
    turn(ENC_ALGORITHM, -1);
    unsigned noise = kit.voice[1].e[FM_NOISE];
    turn(ENC_K2, -1);
    CHECK(kit.voice[1].e[FM_NOISE] == noise - 1);

    /* A step toggled while editing changes the pattern but draws no cell. */
    int was = pattern.voices[1].steps[0].on;
    next.keys = 1u << 0;
    scan();
    next.keys = 0;
    scan();
    CHECK(pattern.voices[1].steps[0].on != was);
    CHECK(cell_fills == 0);

    /* EDIT -> SHAPE: first page FILTER, F.TYP steps one choice per detent. */
    press(BTN_EDIT);
    CHECK(kit.voice[1].c[DC_FTYPE] * 4 / 128 == 0);
    turn(ENC_K1, 1);
    CHECK(kit.voice[1].c[DC_FTYPE] * 4 / 128 == 1);
    turn(ENC_K1, 1);
    CHECK(kit.voice[1].c[DC_FTYPE] * 4 / 128 == 2);
    screenshot("ui_filter");
    /* ALGORITHM -> AMP: KNOB2 is PAN. */
    turn(ENC_ALGORITHM, 1);
    unsigned pan = kit.voice[1].c[DC_PAN];
    turn(ENC_K2, 1);
    CHECK(kit.voice[1].c[DC_PAN] == pan + 1);

    /* EDIT -> TIMING: KNOB1 is the voice length. */
    press(BTN_EDIT);
    unsigned len = pattern.voices[1].length;
    turn(ENC_K1, -1);
    CHECK(pattern.voices[1].length == len - 1);
    screenshot("ui_timing");
    CHECK(cell_fills == 0);

    /* SELECT to CH while in TIMING keeps the mode: KNOB1 is CH's length. */
    turn(ENC_SELECT, 1);
    len = pattern.voices[2].length;
    turn(ENC_K1, -1);
    CHECK(pattern.voices[2].length == len - 1);

    /* EDIT -> back to SEQ: the grid is drawn again. */
    press(BTN_EDIT);
    CHECK(cell_fills >= SEQ_VOICES * 16);

    /* ENGINE mode on BD (SAMPLE: one engine page only): ALGORITHM stays put,
     * switching the engine to FM on KNOB1 opens the other pages. */
    turn(ENC_SELECT, -1);
    turn(ENC_SELECT, -1);
    press(BTN_EDIT);
    CHECK(kit.voice[0].engine == DRUM_SAMPLE);
    turn(ENC_ALGORITHM, 1);
    tune = kit.voice[0].e[E_TUNE];
    turn(ENC_K2, 1);
    CHECK(kit.voice[0].e[E_TUNE] == tune + 1);    /* still page 1 */
    turn(ENC_K1, 1);
    CHECK(kit.voice[0].engine == DRUM_FM);
    turn(ENC_ALGORITHM, 1);
    pdec = kit.voice[0].e[FM_PDEC];
    turn(ENC_K1, 1);
    CHECK(kit.voice[0].e[FM_PDEC] == pdec + 1);    /* page 2 exists now */

    /* Holding a step in an edit mode shows its settings full screen. */
    next.keys = 1u << 2;                          /* step 2 */
    scan();
    turn(ENC_K1, -10);
    CHECK(pattern.voices[0].steps[1].velocity == 80);   /* fast turn: 2x */
    screenshot("ui_step");
    next.keys = 0;
    scan();

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("drum ui ok\n");
    return 0;
}
