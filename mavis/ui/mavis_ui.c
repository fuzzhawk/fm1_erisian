/* SPDX-License-Identifier: MIT */
/* MAVIS panel and screens. All 27 keys play. EDIT cycles PLAY (the live
 * visualiser and a quick strip), SOUND (the engine's pages) and KEYS (tuning,
 * scale, key map, voices); ALGORITHM turns pages; KNOB1..KNOB4 edit the four
 * parameters on screen; LFO held presses the strings (oshide); OCT-/OCT+
 * move the keyboard; SAVE stores the current sound; HOME shows status.
 * Control task only. */
#include <string.h>

#include "../mavis.h"
#include "fm1.h"
#include "ui/draw.h"

enum { BTN_OCT_DOWN, BTN_OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT,
       BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC };
enum { ENC_SELECT, ENC_ALGORITHM, ENC_K1, ENC_K2, ENC_K3, ENC_K4, ENC_PRESETS };

#define KEYS 27u
#define ACCENT RGB565(255, 92, 56)         /* KOTO: vermilion */
#define C_GRID RGB565(40, 22, 18)

#define COL_W 60u
#define TITLE_Y 26u
#define TABS_Y 46u
#define NAME_Y 60u
#define BAR_Y 82u
#define BAR_H 110u
#define BAR_W 30u
#define VALUE_Y 200u
#define HINT_Y 228u
#define VIS_Y 46u
#define VIS_H 96u
#define QUICK_Y 150u

static unsigned mode, param_page;
static int octave;
static uint8_t key_note[KEYS];
static char message[28];
static uint32_t message_until;

/* --- pages ---------------------------------------------------------------- */

enum { PK_NONE, PK_P, PK_TUNING, PK_SCALE, PK_MAP, PK_ROOT, PK_VOICES };
struct slot { uint8_t kind, param; const char *name; };
#define PAGES 9u
#define PAGE_QUICK 8u
static const struct slot pages[PAGES][4] = {
    { { PK_P, KT_DECAY, 0 }, { PK_P, KT_BRIGHT, 0 }, { PK_P, KT_STIFF, 0 }, { PK_P, KT_DAMP, 0 } },
    { { PK_P, KT_POS, 0 }, { PK_P, KT_NAIL, 0 }, { PK_P, KT_STRENGTH, 0 }, { PK_P, KT_NOISE, 0 } },
    { { PK_P, KT_BODY, 0 }, { PK_P, KT_SIZE, 0 }, { PK_P, KT_SAWARI, 0 }, { PK_P, KT_TENSION, 0 } },
    { { PK_P, KT_OSHIDE, 0 }, { PK_P, KT_PRESS, 0 }, { PK_P, KT_BENDIN, 0 }, { PK_P, KT_BENDTIME, 0 } },
    { { PK_P, KT_ATO, 0 }, { PK_P, KT_ATODELAY, 0 }, { PK_P, KT_YURI, 0 }, { PK_P, KT_YURIRATE, 0 } },
    { { PK_P, KT_HUMAN, 0 }, { PK_P, KT_WIDTH, 0 }, { PK_P, KT_LEVEL, 0 }, { PK_NONE, 0, 0 } },
    { { PK_TUNING, 0, "TUNE" }, { PK_SCALE, 0, "SCALE" }, { PK_MAP, 0, "MAP" }, { PK_ROOT, 0, "ROOT" } },
    { { PK_VOICES, 0, "VOICE" }, { PK_NONE, 0, 0 }, { PK_NONE, 0, 0 }, { PK_NONE, 0, 0 } },
    { { PK_P, KT_DECAY, 0 }, { PK_P, KT_BRIGHT, 0 }, { PK_P, KT_POS, 0 }, { PK_P, KT_NAIL, 0 } },
};
static const char *const page_names[PAGES] = {
    "STRING", "PLUCK", "BODY", "PRESS", "MOTION", "OUTPUT", "TUNING", "VOICES", "QUICK",
};
enum { MODE_PLAY, MODE_SOUND, MODE_KEYS, MODES };
static const struct { const char *name; uint8_t first, count; } modes[MODES] = {
    { "PLAY", PAGE_QUICK, 1 }, { "SOUND", 0, 6 }, { "KEYS", 6, 2 },
};
static const char *const note_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void slot_value(const struct slot *s, char *text, unsigned *bar, unsigned *max)
{
    text[0] = 0;
    *bar = 0; *max = 0;
    switch (s->kind) {
    case PK_NONE: return;
    case PK_P:
        *bar = patch.p[s->param]; *max = 127;
        fmt_uint(text, patch.p[s->param]);
        return;
    case PK_TUNING: fmt_str(text, mv_tuning_name(global.tuning)); *bar = global.tuning; *max = TU_COUNT - 1; return;
    case PK_SCALE: fmt_str(text, mv_scale_name(global.scale)); *bar = global.scale; *max = SC_COUNT - 1; return;
    case PK_MAP: fmt_str(text, mv_map_name(global.map)); *bar = global.map; *max = MAP_COUNT - 1; return;
    case PK_ROOT: fmt_str(text, note_names[global.root % 12]); *bar = global.root; *max = 11; return;
    case PK_VOICES: fmt_uint(text, global.voices); *bar = global.voices; *max = MV_MAX_VOICES; return;
    }
}

static const char *slot_name(const struct slot *s)
{
    if (s->kind == PK_P) {
        const char *n = mv_param_name(patch.engine, s->param);
        return n ? n : "";
    }
    return s->name ? s->name : "";
}

static void slot_edit(const struct slot *s, int delta)
{
    int dir = delta > 0 ? 1 : -1;
    switch (s->kind) {
    case PK_NONE: return;
    case PK_P: patch.p[s->param] = (uint8_t)clampi(patch.p[s->param] + delta, 0, 127); break;
    case PK_TUNING: global.tuning = (uint8_t)clampi(global.tuning + dir, 0, TU_COUNT - 1); break;
    case PK_SCALE: global.scale = (uint8_t)clampi(global.scale + dir, 0, SC_COUNT - 1); break;
    case PK_MAP: audio_all_off(); global.map = (uint8_t)clampi(global.map + dir, 0, MAP_COUNT - 1); break;
    case PK_ROOT: global.root = (uint8_t)((global.root + 12 + dir) % 12); break;
    case PK_VOICES: global.voices = (uint8_t)clampi(global.voices + dir, 1, MV_MAX_VOICES); break;
    }
    mv_prepare(&engine, &patch, &global);
}

static unsigned visible_index(unsigned *count)
{
    *count = modes[mode].count;
    return param_page - modes[mode].first;
}

static void page_step(int dir)
{
    const unsigned first = modes[mode].first, n = modes[mode].count;
    param_page = first + (param_page - first + (dir > 0 ? 1u : n - 1u)) % n;
}

/* --- drawing -------------------------------------------------------------- */

static char *fmt_signed(char *out, int v)
{
    if (v > 0) out = fmt_str(out, "+");
    else if (v < 0) { out = fmt_str(out, "-"); v = -v; }
    return fmt_uint(out, (uint32_t)v);
}

static void draw_header(void)
{
    char t[40], *p = fmt_str(t, "MAVIS  ");
    p = fmt_str(p, mv_engine_name(patch.engine));
    p = fmt_str(p, "  OCT");
    fmt_signed(p, octave);
    draw_line(2, 4, t, 1, ACCENT, COLOR_BG);
    draw_line(2, 13, (int32_t)(message_until - fm1_millis()) > 0 ? message : status_text(),
              1, COLOR_DIM, COLOR_BG);
}

static void draw_tabs(void)
{
    unsigned x = 2;
    for (unsigned m = 0; m < MODES; m++) {
        x = draw_text(x, TABS_Y, modes[m].name, 1, m == mode ? ACCENT : COLOR_DIM, COLOR_BG);
        x = draw_text(x, TABS_Y, "  ", 1, COLOR_DIM, COLOR_BG);
    }
}

static void draw_columns(unsigned name_y, unsigned value_y, int bars)
{
    for (unsigned k = 0; k < 4; k++) {
        const struct slot *s = &pages[param_page][k];
        char value[12];
        unsigned bar, max;
        slot_value(s, value, &bar, &max);
        ui_field(k * COL_W, name_y, COL_W, slot_name(s), COLOR_DIM);
        if (bars) ui_bar(k * COL_W + (COL_W - BAR_W) / 2u, BAR_Y, BAR_W, BAR_H, bar, max, 0, ACCENT);
        ui_field(k * COL_W, value_y, COL_W, value, COLOR_TEXT);
    }
}

static void draw_edit(void)
{
    char t[24], *p;
    draw_line(2, TITLE_Y, page_names[param_page], 2, ACCENT, COLOR_BG);
    unsigned count, at = visible_index(&count);
    if (count > 1) {
        p = fmt_uint(t, at + 1);
        p = fmt_str(p, "/");
        fmt_uint(p, count);
        draw_text(FM1_SCREEN_WIDTH - 2u - (unsigned)strlen(t) * 12u, TITLE_Y, t, 2, COLOR_DIM, COLOR_BG);
    }
    draw_tabs();
    draw_columns(NAME_Y, VALUE_Y, 1);
    draw_line(2, HINT_Y, "EDIT:MODE  ALG:PAGE  LFO:PRESS", 1, COLOR_DIM, COLOR_BG);
}

static void draw_play(void)
{
    draw_columns(QUICK_Y, QUICK_Y + 18, 0);
    draw_line(2, 196, "LFO HELD: PRESS (OSHIDE)", 1, COLOR_DIM, COLOR_BG);
    draw_line(2, 208, "EDIT: SOUND   SAVE: KEEP SOUND", 1, COLOR_DIM, COLOR_BG);
    draw_line(2, 220, "HOME: CPU AND STATUS", 1, COLOR_DIM, COLOR_BG);
}

static void draw_body(void)
{
    if (mode == MODE_PLAY) draw_play(); else draw_edit();
}

void ui_redraw(void)
{
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_header();
    if (mode == MODE_PLAY) draw_line(2, 26, "", 1, COLOR_DIM, COLOR_BG);
    draw_body();
}

void ui_say(const char *text)
{
    fmt_str(message, text);
    message_until = fm1_millis() + 1500u;
    draw_header();
}

/* Live visualiser: the newest string's shape over one period, drawn in
 * bands (16 rows per blit). Reads the audio engine's delay line without
 * locking: a torn frame only draws one odd line. */
void ui_visual(uint32_t now)
{
    static uint32_t last;
    static uint16_t band[240 * 16];
    static int16_t ys[240];
    if (mode != MODE_PLAY || now - last < 50u) return;
    last = now;

    const unsigned vi = engine.last_voice < MV_MAX_VOICES ? engine.last_voice : 0;
    const struct mv_voice *v = &engine.voice[vi];
    const int16_t *line = engine.line[vi];
    const uint32_t w = v->w, period = v->pint ? v->pint : 1;
    int32_t peak = 1500;
    static int32_t sample[240];
    for (unsigned x = 0; x < 240; x++) {
        int32_t s = v->active ? line[(w - 1u - x * period / 240u) & (MV_KS_LEN - 1u)] : 0;
        sample[x] = s;
        if (s > peak) peak = s;
        if (-s > peak) peak = -s;
    }
    const int mid = VIS_H / 2;
    for (unsigned x = 0; x < 240; x++)
        ys[x] = (int16_t)(mid - sample[x] * (mid - 4) / peak);

    for (unsigned b = 0; b < VIS_H; b += 16) {
        unsigned rows = VIS_H - b < 16 ? VIS_H - b : 16;
        for (unsigned r = 0; r < rows; r++) {
            int y = (int)(b + r);
            uint16_t base = y == mid ? C_GRID : COLOR_BG;
            for (unsigned x = 0; x < 240; x++) {
                int y0 = ys[x], y1 = x ? ys[x - 1] : ys[x];
                int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
                band[r * 240 + x] = y >= lo - 1 && y <= hi + 1 ? ACCENT : base;
            }
        }
        fm1_screen_blit(0, VIS_Y + b, 240, rows, band, 240u * rows);
    }
}

/* --- input ---------------------------------------------------------------- */

void ui_init(void)
{
    mode = MODE_PLAY;
    param_page = PAGE_QUICK;
    memset(key_note, 0xFF, sizeof key_note);
}

static void set_mode(unsigned m)
{
    mode = m % MODES;
    param_page = modes[mode].first;
    ui_redraw();
}

static void update_leds(uint32_t keys, int pressing)
{
    for (unsigned k = 0; k < KEYS; k++) fm1_led_set(14 + k, keys >> k & 1);
    fm1_led_set(BTN_LFO, pressing);
    fm1_led_set(BTN_EDIT, mode != MODE_PLAY);
}

int ui_poll(uint32_t now)
{
    static uint32_t last_keys, last_buttons, last_body;
    static int dirty;
    struct fm1_controls c;
    fm1_controls_read(&c);
    audio_set_master(c.knob);

    const uint32_t keys_down = c.keys & ~last_keys, keys_up = last_keys & ~c.keys;
    const uint32_t buttons_down = c.buttons & ~last_buttons;

    for (unsigned k = 0; k < KEYS; k++) {
        if (keys_up >> k & 1 && key_note[k] != 0xFF) {
            audio_note(key_note[k], 0);
            key_note[k] = 0xFF;
        }
        if (keys_down >> k & 1) {
            int n = mv_key_note(&global, k, octave);
            if (n >= 0) { key_note[k] = (uint8_t)n; audio_note((unsigned)n, 100); }
        }
    }
    const int pressing = c.buttons >> BTN_LFO & 1;
    audio_press(pressing);

    if (buttons_down >> BTN_OCT_DOWN & 1) { octave = clampi(octave - 1, -3, 3); draw_header(); }
    if (buttons_down >> BTN_OCT_UP & 1) { octave = clampi(octave + 1, -3, 3); draw_header(); }
    if (buttons_down >> BTN_EDIT & 1) set_mode(mode + 1);
    if (buttons_down >> BTN_SAVE & 1) sound_save();

    for (unsigned e = 0; e < 7; e++) {
        int d = c.encoders[e];
        if (!d) continue;
        if (d > 1 || d < -1) d *= 2;                 /* fast turns go further */
        if (e == ENC_ALGORITHM) { if (mode != MODE_PLAY) page_step(d); }
        else if (e >= ENC_K1 && e <= ENC_K4) slot_edit(&pages[param_page][e - ENC_K1], d);
        else continue;                               /* SELECT, PRESETS: morph, patches (later) */
        dirty = 1;
    }
    if (now - last_body >= 500u) dirty = 1;          /* CPU / voices in the header */
    if (dirty && now - last_body >= 60u) {
        draw_header();
        draw_body();
        dirty = 0;
        last_body = now;
    }
    update_leds(c.keys, pressing);
    last_keys = c.keys;
    last_buttons = c.buttons;
    return buttons_down >> BTN_HOME & 1 ? UI_HOME : UI_STAY;
}
