/* SPDX-License-Identifier: MIT */
/* Drum mode: the all-voices grid, voice select on black keys, steps on white
 * keys, parameter pages on the 7 encoders. Control task only. */
#include <string.h>

#include "../erisian.h"
#include "draw.h"
#include "fm1.h"

/* Panel (verified on Charles's unit). */
enum { BTN_OCT_DOWN, BTN_OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT,
       BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC };
enum { ENC_SELECT, ENC_ALGORITHM, ENC_K1, ENC_K2, ENC_K3, ENC_K4, ENC_PRESETS };

/* Keys 0..26 = F3..G5: the 11 black keys pick voices, the 16 white are steps. */
static const uint8_t black_keys[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };
static const uint8_t white_keys[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
static const char *const voice_labels[11] = {
    "BD", "SD", "CH", "OH", "LT", "HT", "CP", "RM", "CB", "FK", "CY",
};

/* Layout. */
#define GRID_X 26u
#define GRID_Y 24u
#define CELL_W 13u
#define CELL_H 12u
#define STRIP_Y 166u

#define C_OFF       RGB565(38, 38, 38)
#define C_OUTSIDE   RGB565(14, 14, 14)
#define C_ON        COLOR_GOLD
#define C_ON_SOFT   RGB565(150, 112, 0)
#define C_HEAD_OFF  RGB565(120, 120, 120)
#define C_HEAD_ON   RGB565(255, 255, 255)

static unsigned voice;          /* selected voice */
static unsigned page;           /* 0..3: steps page*16 .. +15 */
static unsigned param_page;     /* 0 sound, 1 FM/noise, 2 voice timing */
static int pads;                /* ENV: black keys also play */
static int held_step = -1;      /* step key held: encoders edit the step */
static uint8_t drawn_head[SEQ_VOICES];
static char message[24];
static uint32_t message_until;

/* --- parameter pages ------------------------------------------------------ */

enum { PK_PARAM, PK_ENGINE, PK_CHOKE, PK_LENGTH, PK_SPEED, PK_SWING, PK_MUTE,
       PK_MASTER_LEN, PK_BPM };
struct slot { uint8_t kind, param; const char *name; };
static const struct slot pages[3][7] = {
    { { PK_ENGINE, 0, "ENGIN" }, { PK_PARAM, DP_LEVEL, "LEVEL" }, { PK_PARAM, DP_TUNE, "TUNE" },
      { PK_PARAM, DP_DECAY, "DECAY" }, { PK_PARAM, DP_PITCH_ENV, "P.ENV" },
      { PK_PARAM, DP_PITCH_DECAY, "P.DEC" }, { PK_PARAM, DP_PAN, "PAN" } },
    { { PK_PARAM, DP_RATIO, "RATIO" }, { PK_PARAM, DP_MOD, "MOD" }, { PK_PARAM, DP_MOD_DECAY, "M.DEC" },
      { PK_PARAM, DP_FEEDBACK, "FDBK" }, { PK_PARAM, DP_NOISE, "NOISE" },
      { PK_PARAM, DP_NOISE_DECAY, "N.DEC" }, { PK_PARAM, DP_NOISE_TONE, "N.TON" } },
    { { PK_BPM, 0, "BPM" }, { PK_MASTER_LEN, 0, "P.LEN" }, { PK_LENGTH, 0, "LEN" },
      { PK_SPEED, 0, "SPEED" }, { PK_SWING, 0, "SWING" }, { PK_CHOKE, 0, "CHOKE" },
      { PK_MUTE, 0, "MUTE" } },
};
static const char *const page_names[3] = { "SOUND", "FM+NOISE", "TIMING" };
static const char *const speed_names[SEQ_SPEED_COUNT] = { "2X", "1X", "3/4", "1/2", "1/4" };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int slot_value(const struct slot *s, char *text)
{
    struct drum_voice_params *vp = &kit.voice[voice];
    struct seq_voice_track *t = &pattern.voices[voice];
    int v = 0;
    switch (s->kind) {
    case PK_PARAM: v = vp->p[s->param]; break;
    case PK_ENGINE: fmt_str(text, drum_engine_name(vp->engine)); return vp->engine;
    case PK_CHOKE: v = vp->choke; if (!v) { fmt_str(text, "-"); return 0; } break;
    case PK_LENGTH: v = t->length; break;
    case PK_SPEED: fmt_str(text, speed_names[t->speed % SEQ_SPEED_COUNT]); return t->speed;
    case PK_SWING: v = t->swing; break;
    case PK_MUTE: fmt_str(text, t->mute ? "ON" : "-"); return t->mute;
    case PK_MASTER_LEN: v = pattern.length; break;
    case PK_BPM: v = pattern.bpm100 / 100; break;
    }
    fmt_uint(text, (uint32_t)v);
    return v;
}

static void slot_edit(const struct slot *s, int delta)
{
    struct drum_voice_params *vp = &kit.voice[voice];
    struct seq_voice_track *t = &pattern.voices[voice];
    switch (s->kind) {
    case PK_PARAM: vp->p[s->param] = (uint8_t)clampi(vp->p[s->param] + delta, 0, 127); break;
    case PK_ENGINE: vp->engine = (uint8_t)clampi(vp->engine + (delta > 0 ? 1 : -1), 0, DRUM_ENGINES - 1); break;
    case PK_CHOKE: vp->choke = (uint8_t)clampi(vp->choke + (delta > 0 ? 1 : -1), 0, DRUM_CHOKE_GROUPS); break;
    case PK_LENGTH: t->length = (uint8_t)clampi(t->length + delta, 1, SEQ_STEPS); break;
    case PK_SPEED: t->speed = (uint8_t)clampi(t->speed + (delta > 0 ? 1 : -1), 0, SEQ_SPEED_COUNT - 1); break;
    case PK_SWING: t->swing = (uint8_t)clampi(t->swing + delta, 50, 75); break;
    case PK_MUTE: t->mute = delta > 0; break;
    case PK_MASTER_LEN: pattern.length = (uint8_t)clampi(pattern.length + delta, 1, SEQ_STEPS); break;
    case PK_BPM:
        pattern.bpm100 = (uint16_t)clampi(pattern.bpm100 + delta * 100, SEQ_BPM_MIN, SEQ_BPM_MAX);
        break;
    }
    if (s->kind == PK_PARAM || s->kind == PK_ENGINE || s->kind == PK_CHOKE)
        drums_prepare_voice(&drums, &kit, voice);
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
    if (step >= t->length) c = C_OUTSIDE;
    else if (head) c = st->on ? C_HEAD_ON : C_HEAD_OFF;
    else if (st->on) c = st->velocity >= 80 ? C_ON : C_ON_SOFT;
    else c = C_OFF;
    fm1_screen_fill(GRID_X + i * CELL_W, GRID_Y + v * CELL_H, CELL_W - 2, CELL_H - 2, c);
}

static void draw_row_label(unsigned v)
{
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

static void draw_strip(void)
{
    char head[32], *p = fmt_str(head, voice_labels[voice]);
    p = fmt_str(p, " ");
    p = fmt_str(p, held_step >= 0 ? "STEP" : page_names[param_page]);
    if (held_step >= 0) { p = fmt_str(p, " "); fmt_uint(p, (uint32_t)held_step + 1); }
    draw_line(2, STRIP_Y, head, 1, COLOR_GOLD, COLOR_BG);
    for (unsigned k = 0; k < 7; k++) {
        char name[8] = "", value[8] = "";
        if (held_step >= 0) {
            const struct seq_step *st = &pattern.voices[voice].steps[held_step];
            static const char *const step_names[7] = { "ON", "VEL", "PROB", "RATCH", "", "", "" };
            fmt_str(name, step_names[k]);
            if (k == 0) fmt_str(value, st->on ? "ON" : "-");
            if (k == 1) fmt_uint(value, st->velocity);
            if (k == 2) fmt_uint(value, st->probability);
            if (k == 3) fmt_uint(value, st->ratchet);
        } else {
            const struct slot *s = &pages[param_page][k];
            fmt_str(name, s->name);
            slot_value(s, value);
        }
        unsigned x = 2 + (k % 4) * 60, y = STRIP_Y + 14 + (k / 4) * 30;
        draw_line(x, y, name, 1, COLOR_DIM, COLOR_BG);
        draw_text(x, y + 10, "      ", 2, COLOR_TEXT, COLOR_BG);
        draw_text(x, y + 10, value, 2, COLOR_TEXT, COLOR_BG);
    }
}

static void draw_grid(void)
{
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
    draw_grid();
    draw_strip();
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
}

/* --- input ---------------------------------------------------------------- */

void drum_ui_init(void)
{
    voice = 0;
    page = 0;
    param_page = 0;
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
    const int glo = c.buttons >> BTN_GLO & 1;

    if (buttons_down >> BTN_PLAY & 1) {
        if (audio_playing()) audio_stop(); else audio_play();
    }
    if (buttons_down >> BTN_OCT_DOWN & 1) { if (page) { page--; draw_grid(); } }
    if (buttons_down >> BTN_OCT_UP & 1) { if (page + 1 < pages_in_pattern()) { page++; draw_grid(); } }
    if (buttons_down >> BTN_EDIT & 1) { param_page = (param_page + 1) % 3; strip_dirty = 1; }
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
            strip_dirty = 1;
        }
    }

    /* White keys: tap toggles a step; hold one and turn encoders to edit it. */
    int held = -1;
    for (unsigned i = 0; i < 16; i++) {
        unsigned step = page * 16 + i;
        if (keys_down >> white_keys[i] & 1) {
            struct seq_step *st = &pattern.voices[voice].steps[step];
            st->on = !st->on;
            draw_cell(voice, i, 0);
            drawn_head[voice] = 0xFF;
        }
        if (c.keys >> white_keys[i] & 1) held = (int)step;
    }
    if (held != held_step) { held_step = held; strip_dirty = 1; }

    /* Encoders: step edit, global (GLO held) or the parameter page. */
    for (unsigned e = 0; e < 7; e++) {
        int d = c.encoders[e];
        if (!d) continue;
        if (d > 1 || d < -1) d *= 2;                 /* fast turns go further */
        if (held_step >= 0) {
            struct seq_step *st = &pattern.voices[voice].steps[held_step];
            if (e == 0) st->on = d > 0;
            if (e == 1) st->velocity = (uint8_t)clampi(st->velocity + d, 1, 127);
            if (e == 2) st->probability = (uint8_t)clampi(st->probability + d, 0, 100);
            if (e == 3) st->ratchet = (uint8_t)clampi(st->ratchet + (d > 0 ? 1 : -1), 1, 4);
            draw_cell(voice, (unsigned)held_step % 16, 0);
            drawn_head[voice] = 0xFF;
        } else if (glo) {
            slot_edit(&pages[2][e], d);
            draw_header();
            if (e == 1 || e == 2) draw_grid();
        } else {
            slot_edit(&pages[param_page][e], d);
            if (param_page == 2 && (e == 1 || e == 2)) draw_grid();
            if (param_page == 2 && e == 0) draw_header();
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
    return buttons_down >> BTN_HOME & 1;
}
