/* SPDX-License-Identifier: MIT */
/* ERISIAN, first build: boot screen, rescue check, panel diagnostics, free RAM,
 * preset store mount, kick upload over SysEx and kick playback.
 *
 * Contexts (see include/fm1.h): groove_screen_init / groove_screen_control run
 * in the control task and may touch flash; fm1_app_sysex runs in the MIDI
 * transport and only copies the frame; fm1_app_render runs in the audio
 * interrupt from RAM and reads RAM only.
 */
#include <stdlib.h>
#include <string.h>

#include "fm1.h"
#include "fm1_app.h"
#include "storage/store.h"
#include "storage/upload.h"
#include "ui/draw.h"

#ifndef ERISIAN_VERSION
#define ERISIAN_VERSION "dev"
#endif

/* ERISIAN uses user flash below stock V15's patch bank (flash 0xEA000, i.e.
 * user offset 0x6E000), so going back to V15 keeps its patches. */
#define ERISIAN_FLASH_BYTES 0x6E000u
#define RESCUE_WINDOW_MS 200u
#define BUTTON_OCT_DOWN 0u

/* Panel bit names from fm1-emu's model of the FM-1 (to be verified here). */
static const char *const button_names[14] = {
    "OCT-", "OCT+", "FX", "SCL", "ENV", "LFO", "EDIT",
    "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC",
};
static const char *const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

/* --- flash and store ----------------------------------------------------- */

static int flash_read(void *c, uint32_t o, void *d, size_t n) { (void)c; return fm1_flash_read(o, d, n); }
static int flash_write(void *c, uint32_t o, const void *d, size_t n) { (void)c; return fm1_flash_write(o, d, n); }
static int flash_erase(void *c, uint32_t o) { (void)c; return fm1_flash_erase(o); }

static struct store_flash flash_dev = { NULL, 0, flash_read, flash_write, flash_erase };
static struct store store;
static struct upload upload;
static int store_state = 1;    /* 1 = not mounted yet, 0 = ok, <0 = error */
static int rescue;

/* --- SysEx mailbox (transport -> control task) --------------------------- */

static uint8_t mailbox[1024];
static volatile size_t mailbox_bytes;
static unsigned mailbox_port;
static uint8_t reply[UPLOAD_REPLY_MAX];
static size_t reply_bytes;
static unsigned reply_port;

void fm1_app_sysex(const uint8_t *message, size_t bytes)
{
    if (mailbox_bytes || bytes > sizeof mailbox)
        return; /* busy: the uploader resends after its timeout */
    memcpy(mailbox, message, bytes);
    mailbox_port = fm1_midi_input_source();
    mailbox_bytes = bytes;
    fm1_control_request();
}

/* --- kick in RAM, played from the audio interrupt ------------------------ */

static uint8_t *kick_record;              /* sample_header + PCM, malloc'd */
static const int16_t *volatile kick_pcm;
static volatile uint32_t kick_frames;
static uint32_t kick_pos = UINT32_MAX;   /* audio interrupt only */
static volatile uint32_t kick_trigger;   /* control task counts, audio follows */
static uint32_t kick_seen;               /* audio interrupt only */
static volatile uint16_t level = 512;
static int kick_state;                    /* 0 none, 1 loaded, <0 error */

FM1_AUDIO_CODE int fm1_app_render(int32_t *output, unsigned frames)
{
    if (!output || frames != FM1_BLOCK_FRAMES)
        return -1;
    const int16_t *pcm = kick_pcm;
    const uint32_t n = kick_frames;
    const uint32_t trigger = kick_trigger;
    if (trigger != kick_seen) { kick_seen = trigger; kick_pos = 0; }
    uint32_t pos = kick_pos;
    const int32_t gain = level; /* 0..1023 */
    for (unsigned i = 0; i < frames; i++) {
        int32_t s = 0;
        if (pcm && pos < n)
            s = ((int32_t)pcm[pos++] * gain) >> 2; /* 16-bit * 10-bit -> Q23 */
        output[2 * i] = s;
        output[2 * i + 1] = s;
    }
    kick_pos = pos;
    return 0;
}

FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    (void)output;
}

/* Wait until the audio interrupt has finished two more blocks (at most
 * ~50 ms), so no block still reads a buffer we are about to free. */
static void wait_audio_blocks(void)
{
    struct fm1_audio_stats s;
    fm1_audio_stats_read(&s);
    const uint32_t first = s.blocks, t0 = fm1_millis();
    do {
        fm1_audio_stats_read(&s);
    } while (s.blocks - first < 2u && fm1_millis() - t0 < 50u);
}

static void kick_unload(void)
{
    if (!kick_record)
        return;
    kick_frames = 0;
    kick_pcm = NULL;
    wait_audio_blocks();
    free(kick_record);
    kick_record = NULL;
}

static void kick_load(void)
{
    kick_unload();
    kick_state = 0;
    uint32_t offset, length;
    if (store_state != 0 || store_locate(&store, UPLOAD_KEY(0), &offset, &length))
        return;
    uint8_t *record = malloc(length);
    if (!record) { kick_state = -1; return; }
    size_t got;
    if (store_read(&store, UPLOAD_KEY(0), record, length, &got) != STORE_OK) {
        free(record);
        kick_state = -2;
        return;
    }
    struct sample_header h;
    memcpy(&h, record, sizeof h);
    if (h.magic != SAMPLE_MAGIC || (uint64_t)h.frames * 2 + sizeof h != got) {
        free(record);
        kick_state = -3;
        return;
    }
    kick_record = record;
    kick_pcm = (const int16_t *)(const void *)(record + sizeof h);
    kick_frames = h.frames;
    kick_state = 1;
}

/* --- free RAM probe ------------------------------------------------------ */

static uint32_t largest_free;

/* Largest single block malloc can give right now, 1 KB resolution, at most
 * 256 KB. Each probe frees at once. */
static uint32_t probe_heap(void)
{
    uint32_t lo = 0, hi = 256;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        void *p = malloc(mid * 1024u);
        if (p) { free(p); lo = mid; } else { hi = mid - 1; }
    }
    return lo * 1024u;
}

/* --- screen -------------------------------------------------------------- */

enum { LINE_BTN = 48, LINE_KEY = 68, LINE_ENC = 88, LINE_KNOB = 108, LINE_RAM = 128,
       LINE_FLASH = 148, LINE_KICK = 168, LINE_USB = 188, LINE_HINT = 220 };

static char upload_text[24] = "USB IDLE";

static void draw_header(void)
{
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_text(8, 6, "ERISIAN", 3, COLOR_GOLD, COLOR_BG);
    char v[32];
    fmt_str(fmt_str(v, "V "), ERISIAN_VERSION);
    draw_line(8, 32, v, 1, COLOR_DIM, COLOR_BG);
}

static void draw_status(void)
{
    char t[32], *p;

    p = fmt_str(t, "RAM ");
    if (largest_free) { p = fmt_uint(p, largest_free / 1024); fmt_str(p, "K FREE"); }
    else fmt_str(p, "...");
    draw_line(8, LINE_RAM, t, 2, COLOR_TEXT, COLOR_BG);

    if (rescue) {
        draw_line(8, LINE_FLASH, "RESCUE MODE", 2, COLOR_RED, COLOR_BG);
        draw_line(8, LINE_KICK, "FLASH UNTOUCHED", 2, COLOR_TEXT, COLOR_BG);
    } else if (store_state == 1) {
        draw_line(8, LINE_FLASH, "FLASH ...", 2, COLOR_TEXT, COLOR_BG);
    } else if (store_state < 0) {
        p = fmt_str(t, "FLASH ERR ");
        fmt_uint(p, (uint32_t)-store_state);
        draw_line(8, LINE_FLASH, t, 2, COLOR_RED, COLOR_BG);
    } else {
        p = fmt_str(t, store.read_only ? "FLASH RO " : "FLASH OK ");
        p = fmt_uint(p, store_free_sectors(&store) * 4);
        fmt_str(p, "K");
        draw_line(8, LINE_FLASH, t, 2, store.read_only ? COLOR_RED : COLOR_TEXT, COLOR_BG);
    }

    if (!rescue) {
        if (kick_state == 1) {
            p = fmt_str(t, "KICK ");
            p = fmt_uint(p, kick_frames);
            fmt_str(p, " FR");
            draw_line(8, LINE_KICK, t, 2, COLOR_GOLD, COLOR_BG);
        } else if (kick_state == 0) {
            draw_line(8, LINE_KICK, "KICK NONE", 2, COLOR_DIM, COLOR_BG);
        } else {
            p = fmt_str(t, "KICK ERR ");
            fmt_uint(p, (uint32_t)-kick_state);
            draw_line(8, LINE_KICK, t, 2, COLOR_RED, COLOR_BG);
        }
    }
    draw_line(8, LINE_USB, upload_text, 2, COLOR_TEXT, COLOR_BG);
    draw_line(8, LINE_HINT, "KEY: PLAY KICK  KNOB: LEVEL", 1, COLOR_DIM, COLOR_BG);
}

void groove_screen_init(void)
{
    fm1_controls_init();
    fm1_screen_init();
    draw_header();
    draw_line(8, LINE_BTN, "BTN -", 2, COLOR_TEXT, COLOR_BG);
    draw_line(8, LINE_KEY, "KEY -", 2, COLOR_TEXT, COLOR_BG);
    draw_line(8, LINE_ENC, "ENC -", 2, COLOR_TEXT, COLOR_BG);
    draw_line(8, LINE_KNOB, "KNOB -", 2, COLOR_TEXT, COLOR_BG);
    draw_status();
}

/* --- control task -------------------------------------------------------- */

static void handle_upload(void)
{
    if (reply_bytes) { /* a reply waiting for room in the MIDI queue */
        if (fm1_midi_send_port(reply_port, reply, reply_bytes) != -2)
            reply_bytes = 0; /* sent, or the port went away */
        return;
    }
    size_t n = mailbox_bytes;
    if (!n)
        return;
    int ours = 0;
    if (!rescue && store_state == 0)
        ours = upload_handle(&upload, mailbox, n, reply, &reply_bytes);
    uint8_t cmd = n > 4 ? mailbox[4] : 0;
    mailbox_bytes = 0;
    if (!ours) { reply_bytes = 0; return; }
    reply_port = mailbox_port;
    if (fm1_midi_send_port(reply_port, reply, reply_bytes) != -2)
        reply_bytes = 0;

    char *p;
    if (cmd == UPLOAD_DATA && store.stream.active) {
        p = fmt_str(upload_text, "USB ");
        p = fmt_uint(p, store.stream.written / 1024);
        p = fmt_str(p, "/");
        p = fmt_uint(p, store.stream.length / 1024);
        fmt_str(p, "K");
    } else if (cmd == UPLOAD_END) {
        kick_load();
        fmt_str(upload_text, kick_state == 1 ? "USB KICK SAVED" : "USB FAILED");
        draw_status();
        return;
    } else if (cmd == UPLOAD_BEGIN) {
        fmt_str(upload_text, store.stream.active ? "USB RECEIVING" : "USB REFUSED");
        kick_unload(); /* frees RAM for the new one; flash copy stays */
    } else if (cmd == UPLOAD_HELLO) {
        fmt_str(upload_text, "USB UPLOADER");
    }
    draw_line(8, LINE_USB, upload_text, 2, COLOR_TEXT, COLOR_BG);
}

static void show_buttons(uint32_t buttons)
{
    char t[32], *p = fmt_str(t, "BTN ");
    unsigned shown = 0;
    for (unsigned b = 0; b < 14 && shown < 2; b++) {
        if (!(buttons >> b & 1)) continue;
        p = fmt_uint(p, b);
        p = fmt_str(p, " ");
        p = fmt_str(p, button_names[b]);
        p = fmt_str(p, " ");
        shown++;
    }
    if (!shown) fmt_str(p, "-");
    draw_line(8, LINE_BTN, t, 2, COLOR_TEXT, COLOR_BG);
}

static void show_keys(uint32_t keys)
{
    char t[32], *p = fmt_str(t, "KEY ");
    unsigned k = 0;
    while (k < 27 && !(keys >> k & 1)) k++;
    if (k == 27) {
        fmt_str(p, "-");
    } else {
        unsigned midi = 53 + k; /* F3 */
        p = fmt_uint(p, k);
        p = fmt_str(p, " ");
        p = fmt_str(p, note_names[midi % 12]);
        fmt_uint(p, midi / 12 - 1);
    }
    draw_line(8, LINE_KEY, t, 2, COLOR_TEXT, COLOR_BG);
}

void groove_screen_control(void)
{
    static uint32_t start, last_read, last_keys, last_buttons, probe_at;
    static int32_t enc_total[7];
    static uint16_t last_knob = 0xFFFFu;
    static int started;
    const uint32_t now = fm1_millis();
    if (!started) { started = 1; start = now; probe_at = now + 3000u; }

    handle_upload();
    if ((uint32_t)(now - last_read) < 20u)
        return;
    last_read = now;

    struct fm1_controls c;
    fm1_controls_read(&c);
    level = c.knob;

    /* Rescue check first: OCT- held at power-on skips every flash access. */
    if (store_state == 1 && !rescue) {
        if (c.buttons >> BUTTON_OCT_DOWN & 1) {
            rescue = 1;
            draw_status();
        } else if ((uint32_t)(now - start) >= RESCUE_WINDOW_MS) {
            uint32_t size = fm1_flash_size();
            if (size > ERISIAN_FLASH_BYTES) size = ERISIAN_FLASH_BYTES;
            flash_dev.size = size / STORE_SECTOR_BYTES * STORE_SECTOR_BYTES;
            int r = store_mount(&store, &flash_dev, 0);
            store_state = r == STORE_OK ? 0 : (r < 0 ? r : -99);
            if (store_state == 0) {
                upload_init(&upload, &store);
                kick_load();
            }
            draw_status();
        }
    }

    if (!largest_free && (int32_t)(now - probe_at) >= 0) {
        largest_free = probe_heap();
        draw_status();
    }

    if (c.buttons != last_buttons) { show_buttons(c.buttons); last_buttons = c.buttons; }
    if (c.keys != last_keys) {
        if ((c.keys & ~last_keys) && kick_frames)
            kick_trigger++; /* any newly pressed key plays the kick */
        show_keys(c.keys);
        last_keys = c.keys;
    }
    for (unsigned e = 0; e < 7; e++) {
        if (!c.encoders[e]) continue;
        enc_total[e] += c.encoders[e];
        char t[32], *p = fmt_str(t, "ENC ");
        p = fmt_uint(p, e);
        p = fmt_str(p, enc_total[e] < 0 ? " -" : " +");
        fmt_uint(p, (uint32_t)(enc_total[e] < 0 ? -enc_total[e] : enc_total[e]));
        draw_line(8, LINE_ENC, t, 2, COLOR_TEXT, COLOR_BG);
    }
    int knob_move = (int)c.knob - (int)last_knob;
    if (knob_move > 8 || knob_move < -8) {
        char t[16];
        fmt_uint(fmt_str(t, "KNOB "), c.knob);
        draw_line(8, LINE_KNOB, t, 2, COLOR_TEXT, COLOR_BG);
        last_knob = c.knob;
    }
    for (unsigned b = 0; b < 14; b++) fm1_led_set(b, (c.buttons >> b) & 1);
    for (unsigned k = 0; k < 27; k++) fm1_led_set(14 + k, (c.keys >> k) & 1);
}
