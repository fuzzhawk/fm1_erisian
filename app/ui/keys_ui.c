/* SPDX-License-Identifier: MIT */
/* Keys mode (cyan): all 27 keys play the three layers. EDIT cycles the view:
 * PLAY (the layer stack, zone map and a quick strip), then the full-screen
 * edit modes ENGINE, SHAPE and LAYER. ALGORITHM turns the pages of the mode,
 * KNOB1..KNOB4 edit the four parameters on screen, SELECT (or SEL) picks the
 * layer, OCT-/OCT+ move the keyboard, PRESETS sets the tempo, SEQ goes back
 * to drum mode. Control task only. */
#include <string.h>

#include "../erisian.h"
#include "draw.h"
#include "fm1.h"

enum { BTN_OCT_DOWN, BTN_OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT,
       BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC };
enum { ENC_SELECT, ENC_ALGORITHM, ENC_K1, ENC_K2, ENC_K3, ENC_K4, ENC_PRESETS };

#define KEYS 27u
#define KEY_BASE 53u            /* key 0 = F3 */
#define ACCENT COLOR_CYAN

#define COL_W 60u
#define TITLE_Y 26u
#define TABS_Y 46u
#define NAME_Y 60u
#define BAR_Y 82u
#define BAR_H 110u
#define BAR_W 30u
#define VALUE_Y 200u
#define HINT_Y 228u
#define ROWS_Y 62u
#define MAP_Y 138u
#define STRIP_Y 166u

static unsigned layer;          /* selected layer */
static unsigned mode;
static unsigned param_page;
static int octave;              /* keyboard octave, -3..+3 */
static uint8_t key_note[KEYS];  /* note each held key started (0xFF none) */
static char message[24];
static uint32_t message_until;

/* --- pages ---------------------------------------------------------------- */

enum { PK_NONE, PK_E, PK_C, PK_ENGINE, PK_MODE, PK_LO, PK_HI, PK_OCT };
struct slot { uint8_t kind, param; const char *name; };
#define PAGES 9u
#define PAGE_QUICK 8u
static const struct slot pages[PAGES][4] = {
    { { PK_ENGINE, 0, "ENGIN" }, { PK_E, 0, 0 }, { PK_E, 1, 0 }, { PK_E, 2, 0 } },
    { { PK_E, 3, 0 }, { PK_E, 4, 0 }, { PK_E, 5, 0 }, { PK_E, 6, 0 } },
    { { PK_E, 7, 0 }, { PK_NONE, 0, 0 }, { PK_NONE, 0, 0 }, { PK_NONE, 0, 0 } },
    { { PK_C, SC_ATTACK, 0 }, { PK_C, SC_DECAY, 0 }, { PK_C, SC_SUSTAIN, 0 }, { PK_C, SC_RELEASE, 0 } },
    { { PK_C, SC_FTYPE, 0 }, { PK_C, SC_CUTOFF, 0 }, { PK_C, SC_RESO, 0 }, { PK_C, SC_FENV, 0 } },
    { { PK_C, SC_FDECAY, 0 }, { PK_C, SC_DRIVE, 0 }, { PK_C, SC_GLIDE, 0 }, { PK_NONE, 0, 0 } },
    { { PK_MODE, 0, "MODE" }, { PK_LO, 0, "LOW" }, { PK_HI, 0, "HIGH" }, { PK_OCT, 0, "OCT" } },
    { { PK_C, SC_LEVEL, 0 }, { PK_C, SC_PAN, 0 }, { PK_NONE, 0, 0 }, { PK_NONE, 0, 0 } },
    { { PK_C, SC_CUTOFF, 0 }, { PK_C, SC_RESO, 0 }, { PK_C, SC_RELEASE, 0 }, { PK_C, SC_LEVEL, 0 } },
};
static const char *const page_names[PAGES] = {
    "ENGINE", "ENGINE 2", "ENGINE 3", "AMP", "FILTER", "TONE", "LAYER", "MIX", "QUICK",
};
enum { MODE_PLAY, MODE_ENGINE, MODE_SHAPE, MODE_LAYER, MODES };
static const struct { const char *name; uint8_t first, count; } modes[MODES] = {
    { "PLAY", PAGE_QUICK, 1 }, { "ENGINE", 0, 3 }, { "SHAPE", 3, 3 }, { "LAYER", 6, 2 },
};
static const char *const ftype_names[4] = { "OFF", "LP", "HP", "BP" };
static const char *const note_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static struct synth_layer_params *L(void) { return &patch.layer[layer]; }

static int slot_present(const struct slot *s)
{
    if (s->kind == PK_NONE) return 0;
    if (s->kind == PK_E) return synth_engine_param_name(L()->engine, s->param) != 0;
    return 1;
}

static int page_visible(unsigned pg)
{
    for (unsigned k = 0; k < 4; k++)
        if (slot_present(&pages[pg][k])) return 1;
    return 0;
}

static void page_step(int dir)
{
    const unsigned first = modes[mode].first, n = modes[mode].count;
    unsigned at = param_page - first;
    for (unsigned tries = 0; tries < n; tries++) {
        at = (at + (dir > 0 ? 1u : n - 1u)) % n;
        if (page_visible(first + at)) { param_page = first + at; return; }
    }
}

static void page_fix(void)
{
    if (page_visible(param_page)) return;
    for (unsigned i = 0; i < modes[mode].count; i++)
        if (page_visible(modes[mode].first + i)) { param_page = modes[mode].first + i; return; }
    param_page = modes[mode].first;
}

static unsigned visible_index(unsigned *count)
{
    unsigned n = 0, at = 0;
    for (unsigned i = 0; i < modes[mode].count; i++) {
        unsigned pg = modes[mode].first + i;
        if (page_visible(pg)) { if (pg == param_page) at = n; n++; }
    }
    *count = n;
    return at;
}

static unsigned choices(const struct slot *s)
{
    if (s->kind == PK_C && s->param == SC_FTYPE) return 4;
    if (s->kind == PK_E && L()->engine == SY_LEAD && s->param == LE_RATIO) return 16;
    return 0;
}

static uint8_t step_choice(uint8_t v, unsigned n, int delta)
{
    int idx = (int)((unsigned)v * n / 128u) + (delta > 0 ? 1 : -1);
    idx = clampi(idx, 0, (int)n - 1);
    return (uint8_t)((unsigned)idx * 128u / n + 64u / n);
}

static const char *slot_name(const struct slot *s)
{
    if (s->kind == PK_E) {
        const char *n = synth_engine_param_name(L()->engine, s->param);
        return n ? n : "";
    }
    if (s->kind == PK_C) return synth_common_name(s->param);
    return s->name ? s->name : "";
}

static char *fmt_note(char *out, unsigned note)
{
    out = fmt_str(out, note_names[note % 12]);
    unsigned o = note / 12;               /* MIDI 60 = C4 */
    if (o == 0) return fmt_str(out, "-1");
    return fmt_uint(out, o - 1);
}

static char *fmt_signed(char *out, int v)
{
    if (v > 0) out = fmt_str(out, "+");
    else if (v < 0) { out = fmt_str(out, "-"); v = -v; }
    return fmt_uint(out, (uint32_t)v);
}

static void slot_value(const struct slot *s, char *text)
{
    const struct synth_layer_params *p = L();
    text[0] = 0;
    unsigned v = 0;
    switch (s->kind) {
    case PK_NONE: return;
    case PK_E:
        if (!slot_present(s)) return;
        v = p->e[s->param];
        if (s->param == SE_TUNE) { fmt_signed(text, ((int)v - 64) * 100 / 64); return; }   /* cents */
        if (p->engine == SY_LEAD && s->param == LE_RATIO) {
            /* table_ratio is Q8: show x.yy */
            static const uint16_t ratio_x100[16] = { 50, 100, 141, 150, 200, 276, 300, 350,
                                                     400, 519, 540, 600, 700, 800, 942, 1100 };
            unsigned r = ratio_x100[v * 16u / 128u];
            char *q = fmt_uint(text, r / 100);
            q = fmt_str(q, ".");
            if (r % 100 < 10) q = fmt_str(q, "0");
            fmt_uint(q, r % 100);
            return;
        }
        break;
    case PK_C:
        v = p->c[s->param];
        if (s->param == SC_FTYPE) { fmt_str(text, ftype_names[v * 4 / 128]); return; }
        if (s->param == SC_FENV || s->param == SC_PAN) { fmt_signed(text, (int)v - 64); return; }
        break;
    case PK_ENGINE: fmt_str(text, synth_engine_name(p->engine)); return;
    case PK_MODE: fmt_str(text, synth_mode_name(p->mode)); return;
    case PK_LO: fmt_note(text, p->lo); return;
    case PK_HI: fmt_note(text, p->hi); return;
    case PK_OCT: fmt_signed(text, p->octave); return;
    }
    fmt_uint(text, v);
}

static void slot_bar(const struct slot *s, unsigned *value, unsigned *max, int *bipolar)
{
    const struct synth_layer_params *p = L();
    *value = 0; *max = 0; *bipolar = 0;
    switch (s->kind) {
    case PK_NONE: return;
    case PK_E:
        if (!slot_present(s)) return;
        *value = p->e[s->param]; *max = 127; *bipolar = s->param == SE_TUNE; return;
    case PK_C:
        *value = p->c[s->param]; *max = 127;
        *bipolar = s->param == SC_FENV || s->param == SC_PAN; return;
    case PK_ENGINE: *value = p->engine; *max = SY_ENGINES - 1; return;
    case PK_MODE: *value = p->mode; *max = SM_MODES - 1; return;
    case PK_LO: *value = p->lo; *max = 127; return;
    case PK_HI: *value = p->hi; *max = 127; return;
    case PK_OCT: *value = (unsigned)(p->octave + 3); *max = 6; *bipolar = 1; return;
    }
}

static void slot_edit(const struct slot *s, int delta)
{
    struct synth_layer_params *p = L();
    int dir = delta > 0 ? 1 : -1;
    unsigned n = choices(s);
    switch (s->kind) {
    case PK_NONE: return;
    case PK_E:
        if (!slot_present(s)) return;
        p->e[s->param] = n ? step_choice(p->e[s->param], n, delta)
                           : (uint8_t)clampi(p->e[s->param] + delta, 0, 127);
        break;
    case PK_C:
        p->c[s->param] = n ? step_choice(p->c[s->param], n, delta)
                           : (uint8_t)clampi(p->c[s->param] + delta, 0, 127);
        break;
    case PK_ENGINE: {
        int e = clampi(p->engine + dir, 0, SY_ENGINES - 1);
        if (e == p->engine) return;
        audio_keys_off();             /* a sounding voice must not change engine */
        synth_layer_engine_defaults(p, (unsigned)e);
        break;
    }
    case PK_MODE:
        audio_keys_off();
        p->mode = (uint8_t)clampi(p->mode + dir, 0, SM_MODES - 1);
        break;
    case PK_LO:
        audio_keys_off();             /* zone changes would strand note-offs */
        p->lo = (uint8_t)clampi(p->lo + delta, 0, p->hi);
        break;
    case PK_HI:
        audio_keys_off();
        p->hi = (uint8_t)clampi(p->hi + delta, p->lo, 127);
        break;
    case PK_OCT:
        audio_keys_off();
        p->octave = (int8_t)clampi(p->octave + dir, -3, 3);
        break;
    }
    synth_prepare_layer(&synth, &patch, layer);
}

/* --- drawing -------------------------------------------------------------- */

static void draw_header(void)
{
    char t[40], *p = fmt_str(t, "KEYS OCT");
    p = fmt_signed(p, octave);
    p = fmt_str(p, "  ");
    p = fmt_uint(p, pattern.bpm100 / 100);
    p = fmt_str(p, " BPM  ");
    fmt_str(p, audio_playing() ? "PLAY" : "STOP");
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

static void slot_texts(unsigned k, char *name, char *value, unsigned *bar, unsigned *max, int *bipolar)
{
    const struct slot *s = &pages[param_page][k];
    name[0] = 0;
    fmt_str(name, slot_name(s));
    slot_value(s, value);
    slot_bar(s, bar, max, bipolar);
}

static void draw_title(void)
{
    char t[40], *p = fmt_str(t, "L");
    p = fmt_uint(p, layer + 1);
    p = fmt_str(p, " ");
    if (param_page < 3) fmt_str(p, synth_engine_name(L()->engine));
    else fmt_str(p, page_names[param_page]);
    draw_line(2, TITLE_Y, t, 2, ACCENT, COLOR_BG);
    unsigned count, at = visible_index(&count);
    if (count > 1) {
        p = fmt_uint(t, at + 1);
        p = fmt_str(p, "/");
        fmt_uint(p, count);
        draw_text(FM1_SCREEN_WIDTH - 2u - (unsigned)strlen(t) * 12u, TITLE_Y, t, 2, COLOR_DIM, COLOR_BG);
    }
}

static void draw_edit(void)
{
    draw_title();
    for (unsigned k = 0; k < 4; k++) {
        char name[12], value[12];
        unsigned bar = 0, max = 0;
        int bipolar = 0;
        slot_texts(k, name, value, &bar, &max, &bipolar);
        ui_field(k * COL_W, NAME_Y, COL_W, name, COLOR_DIM);
        ui_bar(k * COL_W + (COL_W - BAR_W) / 2u, BAR_Y, BAR_W, BAR_H, bar, max, bipolar, ACCENT);
        ui_field(k * COL_W, VALUE_Y, COL_W, value, COLOR_TEXT);
    }
}

/* PLAY view: the three layers, their zones on the note range, quick strip. */
static void draw_play(void)
{
    char t[40], *p;
    for (unsigned l = 0; l < SYNTH_LAYERS; l++) {
        const struct synth_layer_params *lp = &patch.layer[l];
        unsigned y = ROWS_Y + l * 24u;
        uint16_t c = l == layer ? ACCENT : lp->engine == SY_OFF ? COLOR_DIM : COLOR_TEXT;
        p = fmt_uint(t, l + 1);
        p = fmt_str(p, " ");
        fmt_str(p, synth_engine_name(lp->engine));
        unsigned end = draw_text(2, y, t, 2, c, COLOR_BG);
        if (end < 150) fm1_screen_fill(end, y, 150 - end, 16, COLOR_BG);
        p = fmt_str(t, synth_mode_name(lp->mode));
        if (lp->octave) { p = fmt_str(p, " "); fmt_signed(p, lp->octave); }
        draw_line(156, y, t, 1, COLOR_DIM, COLOR_BG);
        p = fmt_note(t, lp->lo);
        p = fmt_str(p, "-");
        fmt_note(p, lp->hi);
        draw_line(156, y + 8, t, 1, COLOR_DIM, COLOR_BG);
        /* Zone map: 0..127 across the screen. */
        unsigned x0 = lp->lo * 240u / 128u, x1 = (lp->hi + 1u) * 240u / 128u;
        fm1_screen_fill(0, MAP_Y + l * 6u, 240, 4, COLOR_BG);
        if (lp->engine != SY_OFF && x1 > x0)
            fm1_screen_fill(x0, MAP_Y + l * 6u, x1 - x0, 4, l == layer ? ACCENT : COLOR_DIM);
    }
    /* Where the keyboard is now. */
    unsigned k0 = (unsigned)((int)KEY_BASE + octave * 12), k1 = k0 + KEYS;
    fm1_screen_fill(0, MAP_Y + 19, 240, 2, COLOR_BG);
    fm1_screen_fill(k0 * 240u / 128u, MAP_Y + 19, (k1 - k0) * 240u / 128u, 2, COLOR_TEXT);

    p = fmt_str(t, "L");
    p = fmt_uint(p, layer + 1);
    p = fmt_str(p, " QUICK");
    draw_line(2, STRIP_Y, t, 1, ACCENT, COLOR_BG);
    for (unsigned k = 0; k < 4; k++) {
        char name[12], value[12];
        unsigned bar, max;
        int bipolar;
        slot_texts(k, name, value, &bar, &max, &bipolar);
        ui_field(k * COL_W, STRIP_Y + 12, COL_W, name, COLOR_DIM);
        ui_field(k * COL_W, STRIP_Y + 30, COL_W, value, COLOR_TEXT);
    }
}

static void draw_body(void)
{
    if (mode == MODE_PLAY) draw_play(); else draw_edit();
}

static void draw_hint(void)
{
    char hint[48];
    char *p = fmt_str(hint, mode == MODE_PLAY ? "EDIT:SOUND  SEL:LAYER  PRE:" : "EDIT:MODE  ALG:PAGE  PRE:");
    fmt_str(fmt_uint(p, pattern.bpm100 / 100), "BPM");
    draw_line(2, mode == MODE_PLAY ? STRIP_Y + 52 : HINT_Y, hint, 1, COLOR_DIM, COLOR_BG);
}

void keys_ui_redraw(void)
{
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_header();
    draw_tabs();
    draw_body();
    draw_hint();
}

void keys_ui_say(const char *text)
{
    fmt_str(message, text);
    message_until = fm1_millis() + 1500u;
    draw_header();
}

static void set_mode(unsigned m)
{
    mode = m % MODES;
    param_page = modes[mode].first;
    page_fix();
    keys_ui_redraw();
}

static void select_layer(unsigned l)
{
    layer = l % SYNTH_LAYERS;
    page_fix();
}

/* --- input ---------------------------------------------------------------- */

void keys_ui_init(void)
{
    mode = MODE_PLAY;
    param_page = PAGE_QUICK;
    memset(key_note, 0xFF, sizeof key_note);   /* state only: drawn on entry */
}

void keys_ui_leave(void)
{
    for (unsigned k = 0; k < KEYS; k++) key_note[k] = 0xFF;
    audio_keys_off();
}

static void update_leds(uint32_t keys)
{
    for (unsigned k = 0; k < KEYS; k++) fm1_led_set(14 + k, keys >> k & 1);
    fm1_led_set(BTN_PLAY, audio_playing());
    fm1_led_set(BTN_EDIT, mode != MODE_PLAY);
    fm1_led_set(BTN_SEQ, 1);              /* lit: keys mode */
    fm1_led_set(BTN_ENV, 0);
}

int keys_ui_poll(uint32_t now)
{
    static uint32_t last_keys, last_buttons, last_body;
    static int dirty;
    struct fm1_controls c;
    fm1_controls_read(&c);
    audio_set_master(c.knob);

    const uint32_t keys_down = c.keys & ~last_keys, keys_up = last_keys & ~c.keys;
    const uint32_t buttons_down = c.buttons & ~last_buttons;
    const int glo = c.buttons >> BTN_GLO & 1;

    /* Notes first: they are what you hear. */
    for (unsigned k = 0; k < KEYS; k++) {
        if (keys_up >> k & 1 && key_note[k] != 0xFF) {
            audio_note(key_note[k], 0);
            key_note[k] = 0xFF;
        }
        if (keys_down >> k & 1) {
            int n = (int)KEY_BASE + (int)k + octave * 12;
            if (n >= 0 && n <= 127) {
                key_note[k] = (uint8_t)n;
                audio_note((unsigned)n, 100);
            }
        }
    }

    if (buttons_down >> BTN_PLAY & 1) {
        if (audio_playing()) audio_stop(); else audio_play();
    }
    if (buttons_down >> BTN_OCT_DOWN & 1) { octave = clampi(octave - 1, -3, 3); dirty = 1; }
    if (buttons_down >> BTN_OCT_UP & 1) { octave = clampi(octave + 1, -3, 3); dirty = 1; }
    if (buttons_down >> BTN_SEL & 1) { select_layer(layer + 1); dirty = 1; }
    if (buttons_down >> BTN_EDIT & 1) set_mode(mode + 1);
    if (buttons_down >> BTN_SAVE & 1) {
        if (project_save(glo) != 0) keys_ui_say("SAVE FAILED");
    }
    if (buttons_down) draw_header();

    for (unsigned e = 0; e < 7; e++) {
        int d = c.encoders[e];
        if (!d) continue;
        if (d > 1 || d < -1) d *= 2;
        if (e == ENC_SELECT) select_layer(layer + (d > 0 ? 1u : SYNTH_LAYERS - 1u));
        else if (e == ENC_ALGORITHM) page_step(d);
        else if (e == ENC_PRESETS) {
            pattern.bpm100 = (uint16_t)clampi(pattern.bpm100 + d * 100, SEQ_BPM_MIN, SEQ_BPM_MAX);
            drums_set_tempo(&drums, &kit, pattern.bpm100);
            draw_header();
        } else {
            const struct slot *sl = &pages[param_page][e - ENC_K1];
            slot_edit(sl, d);
            if (sl->kind == PK_ENGINE) page_fix();
        }
        dirty = 1;
    }
    if (dirty && now - last_body >= 60u) {
        draw_body();
        draw_hint();
        if (mode == MODE_PLAY) draw_header();
        dirty = 0;
        last_body = now;
    }

    update_leds(c.keys);
    last_keys = c.keys;
    last_buttons = c.buttons;
    if (buttons_down >> BTN_SEQ & 1) return UI_SWITCH;
    return buttons_down >> BTN_HOME & 1 ? UI_HOME : UI_STAY;
}
