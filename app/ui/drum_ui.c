/* SPDX-License-Identifier: MIT */
/* Drum mode. Voice select on black keys (or SELECT), steps on white keys.
 * EDIT cycles the view: SEQ (the all-voices grid with a quick strip), then
 * the full-screen edit modes ENGINE, SHAPE and TIMING. ALGORITHM turns the
 * pages of the current mode (in SEQ: the pattern page), KNOB1..KNOB4 edit the
 * four parameters on screen, PRESETS sets the tempo. Control task only. */
#include <string.h>

#include "../erisian.h"
#include "draw.h"
#include "../engine/plock.h"
#include "fm1.h"

/* Panel (verified on Charles's unit). */
enum { BTN_OCT_DOWN, BTN_OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT,
       BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC };
enum { ENC_SELECT, ENC_ALGORITHM, ENC_K1, ENC_K2, ENC_K3, ENC_K4, ENC_PRESETS };

/* Keys 0..26 = F3..G5: the 11 black keys pick voices, the 16 white are steps. */
static const uint8_t black_keys[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };
static const uint8_t white_keys[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
static const char *const voice_labels[11] = {
    "BD", "SD", "CH", "OH", "LT", "HT", "CP", "WD", "SH", "FK", "RV",
};

/* Layout. */
#define GRID_X 26u
#define GRID_Y 24u
#define CELL_W 13u
#define CELL_H 12u
#define STRIP_Y 166u
/* Full-screen edit view: four columns, one per knob. */
#define COL_W 60u
#define TITLE_Y 26u
#define TABS_Y 46u
#define NAME_Y 60u
#define BAR_Y 82u
#define BAR_H 110u
#define BAR_W 30u
#define VALUE_Y 200u
#define HINT_Y 228u

#define C_OFF       RGB565(38, 38, 38)
#define C_OUTSIDE   RGB565(14, 14, 14)
#define C_ON        COLOR_GOLD
#define C_ON_SOFT   RGB565(150, 112, 0)
#define C_HEAD_OFF  RGB565(120, 120, 120)
#define C_HEAD_ON   RGB565(255, 255, 255)

static unsigned voice;          /* selected voice */
static unsigned page;           /* 0..3: steps page*16 .. +15 */
static unsigned mode;           /* see modes[] */
static unsigned param_page;     /* see page_names */
static int pads;                /* ENV: black keys also play */
static int held_step = -1;      /* step key held: encoders edit the step */
static int held_edited;         /* a knob moved while it was held: no toggle */
static uint8_t drawn_head[SEQ_VOICES];
static char message[24];
static uint32_t message_until;

/* --- parameter pages ------------------------------------------------------ */

/* Pages of four parameters on KNOB1..KNOB4, drawn left to right like the
 * knobs above the buttons. Pages are grouped into modes: EDIT moves to the
 * next mode, ALGORITHM to the next page inside it. Engine pages are labelled
 * by the selected voice's engine and skipped when that engine has nothing
 * there. */
enum { PK_NONE, PK_E, PK_C, PK_ENGINE, PK_CHOKE, PK_LENGTH, PK_SPEED, PK_SWING,
       PK_MUTE, PK_MASTER_LEN };
struct slot { uint8_t kind, param; const char *name; };
#define PAGES 10u
#define PAGE_QUICK 9u
static const struct slot pages[PAGES][4] = {
    { { PK_ENGINE, 0, "ENGIN" }, { PK_E, 0, 0 }, { PK_E, 1, 0 }, { PK_E, 2, 0 } },
    { { PK_E, 3, 0 }, { PK_E, 4, 0 }, { PK_E, 5, 0 }, { PK_E, 6, 0 } },
    { { PK_E, 7, 0 }, { PK_E, 8, 0 }, { PK_E, 9, 0 }, { PK_E, 10, 0 } },
    { { PK_E, 11, 0 }, { PK_E, 12, 0 }, { PK_NONE, 0, 0 }, { PK_NONE, 0, 0 } },
    { { PK_C, DC_FTYPE, 0 }, { PK_C, DC_CUTOFF, 0 }, { PK_C, DC_RESO, 0 }, { PK_C, DC_FENV, 0 } },
    { { PK_C, DC_LEVEL, 0 }, { PK_C, DC_PAN, 0 }, { PK_C, DC_DRIVE, 0 }, { PK_C, DC_FDECAY, 0 } },
    { { PK_C, DC_BITS, 0 }, { PK_C, DC_RATE, 0 }, { PK_C, DC_TRANS, 0 }, { PK_C, DC_TLEVEL, 0 } },
    { { PK_C, DC_TEXTURE, 0 }, { PK_C, DC_XLEVEL, 0 }, { PK_CHOKE, 0, "CHOKE" }, { PK_MUTE, 0, "MUTE" } },
    { { PK_LENGTH, 0, "LEN" }, { PK_SPEED, 0, "SPEED" }, { PK_SWING, 0, "SWING" }, { PK_MASTER_LEN, 0, "P.LEN" } },
    { { PK_E, E_TUNE, 0 }, { PK_E, E_DECAY, 0 }, { PK_C, DC_CUTOFF, 0 }, { PK_C, DC_LEVEL, 0 } },
};
static const char *const page_names[PAGES] = {
    "ENGINE", "ENGINE 2", "ENGINE 3", "ENGINE 4", "FILTER", "AMP", "LO-FI", "TEXTURE", "TIMING",
    "QUICK",
};

/* EDIT cycles these. SEQ shows the grid; the others take the whole screen. */
enum { MODE_SEQ, MODE_ENGINE, MODE_SHAPE, MODE_TIMING, MODES };
static const struct { const char *name; uint8_t first, count; } modes[MODES] = {
    { "SEQ", PAGE_QUICK, 1 }, { "ENGINE", 0, 4 }, { "SHAPE", 4, 4 }, { "TIMING", 8, 1 },
};
static const char *const speed_names[SEQ_SPEED_COUNT] = { "2X", "1X", "3/4", "1/2", "1/4" };
static const char *const ftype_names[4] = { "OFF", "LP", "HP", "BP" };
static const char *const trans_names[4] = { "OFF", "CLICK", "SNAP", "THUMP" };
static const char *const texture_names[4] = { "OFF", "CRACK", "HISS", "BOTH" };
static const char *const excite_names[4] = { "NOISE", "CLICK", "KICK", "MALLT" };
static const char *const material_names[5] = { "WOOD", "PLAST", "TIN", "BOTTL", "GLASS" };
static const char *const noise_modes[3] = { "RISE", "FALL", "IMPCT" };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int slot_present(const struct slot *s)
{
    if (s->kind == PK_NONE) return 0;
    if (s->kind == PK_E) return drum_engine_param_name(kit.voice[voice].engine, s->param) != 0;
    return 1;
}

static int page_visible(unsigned pg)
{
    for (unsigned k = 0; k < 4; k++)
        if (slot_present(&pages[pg][k])) return 1;
    return 0;
}

/* Step to the next/previous page of this mode that has something for the
 * voice (wrapping). */
static void page_step(int dir)
{
    const unsigned first = modes[mode].first, n = modes[mode].count;
    unsigned at = param_page - first;
    for (unsigned tries = 0; tries < n; tries++) {
        at = (at + (dir > 0 ? 1u : n - 1u)) % n;
        if (page_visible(first + at)) { param_page = first + at; return; }
    }
}

/* After a voice or engine change the page may have emptied: fall back to the
 * mode's first page with something on it. */
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

/* Choice parameters: number of choices (0 = a plain 0..127 value). They move
 * one choice per detent; the stored value sits in the middle of its range. */
static unsigned choices(const struct slot *s)
{
    const struct drum_voice_params *vp = &kit.voice[voice];
    if (s->kind == PK_C)
        return s->param == DC_FTYPE || s->param == DC_TRANS || s->param == DC_TEXTURE ? 4u : 0u;
    if (s->kind != PK_E) return 0;
    if (vp->engine == DRUM_KS && s->param == KS_EXCITE) return 4;
    if (vp->engine == DRUM_OBJECT && s->param == OB_MATERIAL) return 5;
    if (vp->engine == DRUM_NOISE && s->param == NZ_MODE) return 3;
    if (vp->engine == DRUM_FM && s->param == FM_RATIO) return 16;
    return 0;
}

static uint8_t step_choice(uint8_t v, unsigned n, int delta)
{
    int idx = (int)((unsigned)v * n / 128u) + (delta > 0 ? 1 : -1);
    idx = clampi(idx, 0, (int)n - 1);
    return (uint8_t)((unsigned)idx * 128u / n + 64u / n);
}

/* Holding a step on an ENGINE or SHAPE page: the knobs lock parameters on
 * that step. Elsewhere a held step shows its own settings. */
static int lock_view(void)
{
    return held_step >= 0 && (mode == MODE_ENGINE || mode == MODE_SHAPE);
}

static int lock_index(const struct slot *s)
{
    if (s->kind == PK_E && slot_present(s)) return (int)PLOCK_E(s->param);
    if (s->kind == PK_C) return (int)PLOCK_C(s->param);
    return -1;
}

/* Stored 0..127 value of an engine/common slot: the kit's, or the held
 * step's lock in the lock view. */
static unsigned slot_raw(const struct slot *s, int *locked)
{
    *locked = 0;
    int li = lock_index(s);
    if (li < 0) return 0;
    if (lock_view())
        return plock_value(&pattern, &kit, voice, (unsigned)held_step, (unsigned)li, locked);
    return s->kind == PK_E ? kit.voice[voice].e[s->param] : kit.voice[voice].c[s->param];
}

/* One knob move of an engine/common value: choices step by one, a few
 * parameters by a coarser amount. */
static uint8_t param_step(const struct slot *s, unsigned v, int delta)
{
    const struct drum_voice_params *vp = &kit.voice[voice];
    unsigned n = choices(s);
    int dir = delta > 0 ? 1 : -1;
    if (n) return step_choice((uint8_t)v, n, delta);
    if (s->kind == PK_E && vp->engine == DRUM_METAL && s->param == MT_REVERSE)   /* 0 = off, 8 per step */
        return (uint8_t)clampi(((int)v / 8 + dir) * 8, 0, 120);
    if (s->kind == PK_E && vp->engine == DRUM_NOISE && s->param == NZ_LENGTH)    /* 2 per step */
        return (uint8_t)clampi(((int)v / 2 + dir) * 2, 0, 127);
    if (s->kind == PK_C && s->param == DC_BITS) return (uint8_t)clampi((int)v + dir * 11, 0, 127);
    if (s->kind == PK_C && s->param == DC_RATE) return (uint8_t)clampi((int)v + dir * 4, 0, 127);
    return (uint8_t)clampi((int)v + delta, 0, 127);
}

static const char *slot_name(const struct slot *s)
{
    if (s->kind == PK_E) {
        const char *n = drum_engine_param_name(kit.voice[voice].engine, s->param);
        return n ? n : "";
    }
    if (s->kind == PK_C) return drum_common_name(s->param);
    return s->name ? s->name : "";
}

static void slot_value(const struct slot *s, char *text)
{
    struct drum_voice_params *vp = &kit.voice[voice];
    struct seq_voice_track *t = &pattern.voices[voice];
    int locked;
    unsigned v = slot_raw(s, &locked);
    text[0] = 0;
    switch (s->kind) {
    case PK_NONE: return;
    case PK_E:
        if (!slot_present(s)) return;
        if (vp->engine == DRUM_KS && s->param == KS_EXCITE) { fmt_str(text, excite_names[v * 4 / 128]); return; }
        if (vp->engine == DRUM_OBJECT && s->param == OB_MATERIAL) { fmt_str(text, material_names[v * 5 / 128]); return; }
        if (vp->engine == DRUM_NOISE && s->param == NZ_MODE) { fmt_str(text, noise_modes[v * 3 / 128]); return; }
        if (vp->engine == DRUM_NOISE && s->param == NZ_LENGTH) { fmt_str(fmt_uint(text, 1u + v / 2u), "ST"); return; }
        if (vp->engine == DRUM_FM && s->param == FM_RATIO) { fmt_uint(fmt_str(text, "#"), v * 16u / 128u); return; }
        if (vp->engine == DRUM_METAL && s->param == MT_REVERSE) {
            if (!v) fmt_str(text, "OFF"); else fmt_str(fmt_uint(text, 1u + v * 16u / 128u), "ST");
            return;
        }
        break;
    case PK_C:
        if (s->param == DC_FTYPE) { fmt_str(text, ftype_names[v * 4 / 128]); return; }
        if (s->param == DC_TRANS) { fmt_str(text, trans_names[v * 4 / 128]); return; }
        if (s->param == DC_TEXTURE) { fmt_str(text, texture_names[v * 4 / 128]); return; }
        if (s->param == DC_BITS) { fmt_str(fmt_uint(text, 16u - (127u - v) * 12u / 127u), "B"); return; }
        if (s->param == DC_RATE) { fmt_uint(fmt_str(text, "/"), 1u + v / 4u); return; }
        break;
    case PK_ENGINE: fmt_str(text, drum_engine_name(vp->engine)); return;
    case PK_CHOKE: if (!vp->choke) { fmt_str(text, "-"); return; } v = vp->choke; break;
    case PK_LENGTH: v = t->length; break;
    case PK_SPEED: fmt_str(text, speed_names[t->speed % SEQ_SPEED_COUNT]); return;
    case PK_SWING: v = t->swing; break;
    case PK_MUTE: fmt_str(text, t->mute ? "ON" : "-"); return;
    case PK_MASTER_LEN: v = pattern.length; break;
    }
    fmt_uint(text, v);
}

/* Where a value sits in its range, for the bar: value 0..max, and whether
 * the bar grows from the middle (pan, filter envelope). */
static void slot_bar(const struct slot *s, unsigned *value, unsigned *max, int *bipolar)
{
    const struct drum_voice_params *vp = &kit.voice[voice];
    const struct seq_voice_track *t = &pattern.voices[voice];
    *value = 0; *max = 0; *bipolar = 0;
    switch (s->kind) {
    case PK_NONE: return;
    case PK_E: if (slot_present(s)) { int l; *value = slot_raw(s, &l); *max = 127; } return;
    case PK_C: {
        int l;
        *value = slot_raw(s, &l); *max = 127;
        *bipolar = s->param == DC_PAN || s->param == DC_FENV;
        return;
    }
    case PK_ENGINE: *value = vp->engine; *max = DRUM_ENGINES - 1; return;
    case PK_CHOKE: *value = vp->choke; *max = DRUM_CHOKE_GROUPS; return;
    case PK_LENGTH: *value = t->length; *max = SEQ_STEPS; return;
    case PK_SPEED: *value = t->speed; *max = SEQ_SPEED_COUNT - 1; return;
    case PK_SWING: *value = t->swing - 50u; *max = 25; return;
    case PK_MUTE: *value = t->mute; *max = 1; return;
    case PK_MASTER_LEN: *value = pattern.length; *max = SEQ_STEPS; return;
    }
}

static void slot_edit(const struct slot *s, int delta)
{
    struct drum_voice_params *vp = &kit.voice[voice];
    struct seq_voice_track *t = &pattern.voices[voice];
    switch (s->kind) {
    case PK_NONE: return;
    case PK_E:
        if (!slot_present(s)) return;
        vp->e[s->param] = param_step(s, vp->e[s->param], delta);
        break;
    case PK_C:
        vp->c[s->param] = param_step(s, vp->c[s->param], delta);
        break;
    case PK_ENGINE: {
        int e = clampi(vp->engine + (delta > 0 ? 1 : -1), 0, DRUM_ENGINES - 1);
        if (e != vp->engine) {
            drum_voice_engine_defaults(vp, (unsigned)e);
            drums_prepare_voice(&drums, &kit, voice);
            plock_clear_engine(&pattern, &kit, &drums, voice);   /* engine locks mean nothing now */
        }
        break;
    }
    case PK_CHOKE: vp->choke = (uint8_t)clampi(vp->choke + (delta > 0 ? 1 : -1), 0, DRUM_CHOKE_GROUPS); break;
    case PK_LENGTH: t->length = (uint8_t)clampi(t->length + delta, 1, SEQ_STEPS); break;
    case PK_SPEED: t->speed = (uint8_t)clampi(t->speed + (delta > 0 ? 1 : -1), 0, SEQ_SPEED_COUNT - 1); break;
    case PK_SWING: t->swing = (uint8_t)clampi(t->swing + delta, 50, 75); break;
    case PK_MUTE: t->mute = delta > 0; break;
    case PK_MASTER_LEN: pattern.length = (uint8_t)clampi(pattern.length + delta, 1, SEQ_STEPS); break;
    }
    if (s->kind == PK_E || s->kind == PK_C || s->kind == PK_ENGINE || s->kind == PK_CHOKE) {
        drums_prepare_voice(&drums, &kit, voice);
        plock_prepare_voice(&pattern, &kit, &drums, voice);
    }
}

/* Lock the slot's parameter on the held step (turning the step on). */
static void lock_edit(const struct slot *s, int delta)
{
    int li = lock_index(s), locked;
    if (li < 0) return;
    unsigned step = (unsigned)held_step;
    unsigned v = plock_value(&pattern, &kit, voice, step, (unsigned)li, &locked);
    if (plock_set(&pattern, &kit, &drums, voice, step, (unsigned)li, param_step(s, v, delta)) != 0) {
        drum_ui_say("LOCK POOL FULL");
        return;
    }
    pattern.voices[voice].steps[step].on = 1;
}

static void tempo_edit(int delta)
{
    pattern.bpm100 = (uint16_t)clampi(pattern.bpm100 + delta * 100, SEQ_BPM_MIN, SEQ_BPM_MAX);
    drums_set_tempo(&drums, &kit, pattern.bpm100);
    plock_tempo(&pattern, &kit, &drums);
}

/* --- drawing -------------------------------------------------------------- */

static unsigned pages_in_pattern(void)
{
    unsigned longest = pattern.length;
    for (unsigned v = 0; v < SEQ_VOICES; v++)
        if (pattern.voices[v].length > longest) longest = pattern.voices[v].length;
    return (longest + 15) / 16;
}

static void draw_cell(unsigned v, unsigned i, int head)
{
    unsigned step = page * 16 + i;
    const struct seq_voice_track *t = &pattern.voices[v];
    const struct seq_step *st = &t->steps[step];
    uint16_t c;
    if (mode != MODE_SEQ) return;              /* the grid is hidden while editing */
    if (step >= t->length) c = C_OUTSIDE;
    else if (head) c = st->on ? C_HEAD_ON : C_HEAD_OFF;
    else if (st->on) c = st->velocity >= 80 ? C_ON : C_ON_SOFT;
    else c = C_OFF;
    fm1_screen_fill(GRID_X + i * CELL_W, GRID_Y + v * CELL_H, CELL_W - 2, CELL_H - 2, c);
    if (st->lock && step < t->length)       /* locked step: a dark notch */
        fm1_screen_fill(GRID_X + i * CELL_W + 4, GRID_Y + v * CELL_H + 3, 3, 4, COLOR_BG);
}

static void draw_row_label(unsigned v)
{
    if (mode != MODE_SEQ) return;
    uint16_t c = v == voice ? COLOR_GOLD : pattern.voices[v].mute ? RGB565(80, 30, 30) : COLOR_DIM;
    draw_text(2, GRID_Y + v * CELL_H + 1, voice_labels[v], 1, c, COLOR_BG);
}

static void draw_header(void)
{
    char t[40], *p = t;
    p = fmt_str(p, "P");
    p = fmt_uint(p, page + 1);
    p = fmt_str(p, "/");
    p = fmt_uint(p, pages_in_pattern());
    p = fmt_str(p, "  ");
    p = fmt_uint(p, pattern.bpm100 / 100);
    p = fmt_str(p, " BPM  ");
    p = fmt_str(p, audio_playing() ? "PLAY" : "STOP");
    if (pads) p = fmt_str(p, " PADS");
    draw_line(2, 4, t, 1, COLOR_GOLD, COLOR_BG);
    draw_text(2, 13, (int32_t)(message_until - fm1_millis()) > 0 ? message : status_text(),
              1, COLOR_DIM, COLOR_BG);
}

#define draw_field(x, y, w, text, fg) ui_field(x, y, w, text, fg)

/* The four knob slots of what is being edited: names and values. */
/* Returns the value colour: gold for a locked value, dim for one the held
 * step only follows from the kit. */
static uint16_t slot_texts(unsigned k, char *name, char *value, unsigned *bar, unsigned *max, int *bipolar)
{
    name[0] = value[0] = 0;
    if (held_step >= 0 && !lock_view()) {
        const struct seq_step *st = &pattern.voices[voice].steps[held_step];
        static const char *const step_names[4] = { "VEL", "PROB", "RATCH", "MICRO" };
        fmt_str(name, step_names[k]);
        *bipolar = 0;
        if (k == 0) { fmt_uint(value, st->velocity); *bar = st->velocity; *max = 127; }
        if (k == 1) { fmt_uint(value, st->probability); *bar = st->probability; *max = 100; }
        if (k == 2) { fmt_uint(value, st->ratchet); *bar = st->ratchet - 1u; *max = 3; }
        if (k == 3) {
            int m = st->micro;
            if (m > 0) fmt_uint(fmt_str(value, "+"), (unsigned)m);
            else if (m < 0) fmt_uint(fmt_str(value, "-"), (unsigned)-m);
            else fmt_str(value, "0");
            *bar = (unsigned)(m + SEQ_MICRO_MAX); *max = 2 * SEQ_MICRO_MAX; *bipolar = 1;
        }
        return COLOR_TEXT;
    }
    const struct slot *s = &pages[param_page][k];
    fmt_str(name, slot_name(s));
    slot_value(s, value);
    slot_bar(s, bar, max, bipolar);
    if (!lock_view()) return COLOR_TEXT;
    int locked;
    slot_raw(s, &locked);
    if (lock_index(s) < 0) { value[0] = 0; *max = 0; }   /* not lockable */
    return locked ? COLOR_GOLD : COLOR_DIM;
}

static void edit_title(char *out)
{
    char *p = fmt_str(out, voice_labels[voice]);
    p = fmt_str(p, " ");
    if (held_step >= 0) {
        p = fmt_str(p, "STEP ");
        p = fmt_uint(p, (uint32_t)held_step + 1);
        if (lock_view()) fmt_str(p, " LOCK");
    } else if (param_page < modes[MODE_ENGINE].first + modes[MODE_ENGINE].count || param_page == PAGE_QUICK) {
        fmt_str(p, drum_engine_name(kit.voice[voice].engine));   /* "SD FM  2/3" */
    } else {
        fmt_str(p, page_names[param_page]);
    }
}

static void draw_bar(unsigned x, unsigned value, unsigned max, int bipolar)
{
    ui_bar(x + (COL_W - BAR_W) / 2u, BAR_Y, BAR_W, BAR_H, value, max, bipolar, COLOR_GOLD);
}

/* Full-screen edit view (ENGINE, SHAPE, TIMING). */
static void draw_edit(void)
{
    char t[40], *p;
    unsigned count, at = visible_index(&count);
    edit_title(t);
    draw_line(2, TITLE_Y, t, 2, COLOR_GOLD, COLOR_BG);
    if (held_step < 0 && count > 1) {
        p = fmt_uint(t, at + 1);
        p = fmt_str(p, "/");
        fmt_uint(p, count);
        draw_text(FM1_SCREEN_WIDTH - 2u - (unsigned)strlen(t) * 12u, TITLE_Y, t, 2, COLOR_DIM, COLOR_BG);
    }
    for (unsigned k = 0; k < 4; k++) {                 /* KNOB1 .. KNOB4 */
        char name[12], value[12];
        unsigned bar = 0, max = 0;
        int bipolar = 0;
        uint16_t vc = slot_texts(k, name, value, &bar, &max, &bipolar);
        draw_field(k * COL_W, NAME_Y, COL_W, name, COLOR_DIM);
        draw_bar(k * COL_W, bar, max, bipolar);
        draw_field(k * COL_W, VALUE_Y, COL_W, value, vc);
    }
}

/* Mode tabs, current one in gold: shows where EDIT goes next. */
static void draw_tabs(void)
{
    unsigned x = 2;
    for (unsigned m = 0; m < MODES; m++) {
        x = draw_text(x, TABS_Y, modes[m].name, 1, m == mode ? COLOR_GOLD : COLOR_DIM, COLOR_BG);
        x = draw_text(x, TABS_Y, "  ", 1, COLOR_DIM, COLOR_BG);
    }
}

/* SEQ view: the quick strip under the grid. */
static void draw_strip(void)
{
    char head[40];
    if (mode != MODE_SEQ) { draw_edit(); return; }
    edit_title(head);
    draw_line(2, STRIP_Y, head, 1, COLOR_GOLD, COLOR_BG);
    for (unsigned k = 0; k < 4; k++) {
        char name[12], value[12];
        unsigned bar, max;
        int bipolar;
        uint16_t vc = slot_texts(k, name, value, &bar, &max, &bipolar);
        draw_field(k * COL_W, STRIP_Y + 12, COL_W, name, COLOR_DIM);
        draw_field(k * COL_W, STRIP_Y + 30, COL_W, value, vc);
    }
}

static void draw_hint(void)
{
    char hint[48], *p;
    if (mode == MODE_SEQ) {
        p = fmt_str(hint, "EDIT:SOUND  ALG:P");
        p = fmt_uint(p, page + 1);
        p = fmt_str(p, "  PRE:");
    } else {
        p = fmt_str(hint, "EDIT:MODE  ALG:PAGE  PRE:");
    }
    fmt_str(fmt_uint(p, pattern.bpm100 / 100), "BPM");
    draw_line(2, mode == MODE_SEQ ? STRIP_Y + 52 : HINT_Y, hint, 1, COLOR_DIM, COLOR_BG);
}

static void draw_grid(void)
{
    if (mode != MODE_SEQ) return;
    for (unsigned v = 0; v < SEQ_VOICES; v++) {
        draw_row_label(v);
        for (unsigned i = 0; i < 16; i++) draw_cell(v, i, 0);
        drawn_head[v] = 0xFF;
    }
}

void drum_ui_redraw(void)
{
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_header();
    if (mode == MODE_SEQ) draw_grid(); else draw_tabs();
    draw_strip();
    draw_hint();
}

static void set_mode(unsigned m)
{
    mode = m % MODES;
    param_page = modes[mode].first;
    page_fix();
    drum_ui_redraw();
}

void drum_ui_say(const char *text)
{
    fmt_str(message, text);
    message_until = fm1_millis() + 1500u;
    draw_header();
}

/* Playheads: each voice has its own (polyrhythms). */
static void update_playheads(void)
{
    if (mode != MODE_SEQ) return;
    for (unsigned v = 0; v < SEQ_VOICES; v++) {
        unsigned step = audio_playing() ? seq_voice_step(&seq, &pattern, v) : 0xFF;
        uint8_t cell = step != 0xFF && step / 16 == page ? (uint8_t)(step % 16) : 0xFF;
        if (cell == drawn_head[v]) continue;
        if (drawn_head[v] != 0xFF) draw_cell(v, drawn_head[v], 0);
        if (cell != 0xFF) draw_cell(v, cell, 1);
        drawn_head[v] = cell;
    }
}

static void update_leds(uint32_t keys)
{
    for (unsigned i = 0; i < 16; i++) {
        unsigned step = page * 16 + i;
        int on = pattern.voices[voice].steps[step].on;
        fm1_led_set(14 + white_keys[i], on || (keys >> white_keys[i] & 1));
    }
    for (unsigned v = 0; v < 11; v++)
        fm1_led_set(14 + black_keys[v], v == voice || (keys >> black_keys[v] & 1));
    fm1_led_set(BTN_PLAY, audio_playing());
    fm1_led_set(BTN_ENV, pads);
    fm1_led_set(BTN_EDIT, mode != MODE_SEQ);
    fm1_led_set(BTN_SEQ, 0);              /* unlit: drum mode */
}

/* --- input ---------------------------------------------------------------- */

/* Pattern page (steps 1-16, 17-32...): OCT-/OCT+, or ALGORITHM in SEQ. */
static void step_page(int dir)
{
    if (dir < 0 && page) page--;
    else if (dir > 0 && page + 1 < pages_in_pattern()) page++;
    else return;
    draw_grid();
    draw_header();
    draw_hint();
}

void drum_ui_init(void)
{
    voice = 0;
    page = 0;
    mode = MODE_SEQ;
    param_page = PAGE_QUICK;
    held_step = -1;
    drum_ui_redraw();
}

int drum_ui_poll(uint32_t now)
{
    static uint32_t last_keys, last_buttons, last_strip;
    static int strip_dirty;
    struct fm1_controls c;
    fm1_controls_read(&c);
    audio_set_master(c.knob);

    const uint32_t keys_down = c.keys & ~last_keys;
    const uint32_t buttons_down = c.buttons & ~last_buttons;
    const int glo = c.buttons >> BTN_GLO & 1;   /* GLO + SAVE = save now */

    if (buttons_down >> BTN_PLAY & 1) {
        if (audio_playing()) audio_stop(); else audio_play();
    }
    if (buttons_down >> BTN_OCT_DOWN & 1) step_page(-1);
    if (buttons_down >> BTN_OCT_UP & 1) step_page(1);
    if (buttons_down >> BTN_EDIT & 1) {
        if (held_step >= 0) {                        /* hold step + EDIT: clear its locks */
            plock_clear_step(&pattern, voice, (unsigned)held_step);
            held_edited = 1;
            draw_cell(voice, (unsigned)held_step % 16, 0);
            drawn_head[voice] = 0xFF;
            drum_ui_say("LOCKS CLEARED");
            strip_dirty = 1;
        } else {
            set_mode(mode + 1);
        }
    }
    if (buttons_down >> BTN_ENV & 1) { pads = !pads; }
    if (buttons_down >> BTN_SAVE & 1) {
        if (project_save(glo) != 0) drum_ui_say("SAVE FAILED");
    }
    if (buttons_down) draw_header();

    /* Black keys: select a voice (and play it in pads mode). */
    for (unsigned v = 0; v < 11; v++) {
        if (!(keys_down >> black_keys[v] & 1)) continue;
        if (pads) audio_pad(v, 110);
        if (v != voice) {
            unsigned old = voice;
            voice = v;
            draw_row_label(old);
            draw_row_label(v);
            page_fix();
            strip_dirty = 1;
        }
    }

    /* White keys: a tap toggles the step when the key comes up. Holding one
     * and turning knobs edits the step (or locks parameters on it) instead;
     * then the release leaves it as it is. Turning a step off drops its
     * locks. */
    const uint32_t keys_up = last_keys & ~c.keys;
    for (unsigned i = 0; i < 16; i++) {
        unsigned step = page * 16 + i;
        if (keys_down >> white_keys[i] & 1) {
            held_step = (int)step;
            held_edited = 0;
            strip_dirty = 1;
        }
        if (keys_up >> white_keys[i] & 1) {
            if (!((int)step == held_step && held_edited)) {
                struct seq_step *st = &pattern.voices[voice].steps[step];
                st->on = !st->on;
                if (!st->on) plock_clear_step(&pattern, voice, step);
                draw_cell(voice, i, 0);
                drawn_head[voice] = 0xFF;
            }
            if ((int)step == held_step) { held_step = -1; strip_dirty = 1; }
        }
    }

    /* Encoders. KNOB1..KNOB4: the page's four parameters (or the held
     * step's). SELECT: voice. ALGORITHM: page of the mode (SEQ: pattern
     * page). PRESETS: tempo. */
    for (unsigned e = 0; e < 7; e++) {
        int d = c.encoders[e];
        if (!d) continue;
        if (d > 1 || d < -1) d *= 2;                 /* fast turns go further */
        if (e == ENC_SELECT) {
            unsigned old = voice;
            voice = (voice + SEQ_VOICES + (d > 0 ? 1u : SEQ_VOICES - 1u)) % SEQ_VOICES;
            draw_row_label(old);
            draw_row_label(voice);
            page_fix();
        } else if (e == ENC_ALGORITHM) {
            if (mode == MODE_SEQ) step_page(d);
            else page_step(d);
        } else if (e == ENC_PRESETS) {
            tempo_edit(d);
            draw_header();
            draw_hint();
        } else {
            unsigned k = e - ENC_K1;                 /* 0..3 */
            if (held_step >= 0) {
                struct seq_step *st = &pattern.voices[voice].steps[held_step];
                held_edited = 1;
                if (lock_view()) {
                    lock_edit(&pages[param_page][k], d);
                } else {
                    if (k == 0) st->velocity = (uint8_t)clampi(st->velocity + d, 1, 127);
                    if (k == 1) st->probability = (uint8_t)clampi(st->probability + d, 0, 100);
                    if (k == 2) st->ratchet = (uint8_t)clampi(st->ratchet + (d > 0 ? 1 : -1), 1, 4);
                    if (k == 3) st->micro = (int8_t)clampi(st->micro + (d > 0 ? 1 : -1),
                                                           -SEQ_MICRO_MAX, SEQ_MICRO_MAX);
                }
                draw_cell(voice, (unsigned)held_step % 16, 0);
                drawn_head[voice] = 0xFF;
            } else {
                const struct slot *sl = &pages[param_page][k];
                slot_edit(sl, d);
                if (sl->kind == PK_LENGTH || sl->kind == PK_MASTER_LEN || sl->kind == PK_MUTE) draw_grid();
                if (sl->kind == PK_ENGINE) page_fix();
            }
        }
        strip_dirty = 1;
    }
    if (strip_dirty && now - last_strip >= 60u) {   /* at most ~16 strip redraws/s */
        draw_strip();
        strip_dirty = 0;
        last_strip = now;
    }

    update_playheads();
    update_leds(c.keys);
    last_keys = c.keys;
    last_buttons = c.buttons;
    if (buttons_down >> BTN_SEQ & 1) return UI_SWITCH;
    return buttons_down >> BTN_HOME & 1 ? UI_HOME : UI_STAY;
}
