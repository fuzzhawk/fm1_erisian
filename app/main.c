/* SPDX-License-Identifier: MIT */
/* ERISIAN: boot, rescue check, flash store, kick upload, project save/load
 * and the status page. The drum machine itself is in ui/drum_ui.c (control
 * task) and audio.c (audio interrupt).
 *
 * Contexts (include/fm1.h): groove_screen_init / groove_screen_control run in
 * the control task and may touch flash; fm1_app_sysex runs in the MIDI
 * transport and only copies the frame.
 */
#include <stdlib.h>
#include <string.h>

#include "erisian.h"
#include "fm1.h"
#include "fm1_app.h"
#include "storage/store.h"
#include "storage/upload.h"
#include "ui/draw.h"

/* ERISIAN uses user flash below stock V15's patch bank (flash 0xEA000, i.e.
 * user offset 0x6E000), so going back to V15 keeps its patches. */
#define ERISIAN_FLASH_BYTES 0x6E000u
#define RESCUE_WINDOW_MS 200u
#define BUTTON_OCT_DOWN 0u
#define BUTTON_HOME 8u

#define PROJECT_KEY 0x01u
#define PROJECT_MAGIC 0x31505245u /* "ERP1" */
#define PROJECT_VERSION 1u

struct project_file {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    struct seq_pattern pattern;
    struct drum_kit kit;
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
static int status_page;
static uint32_t largest_free;
static char upload_text[24] = "USB IDLE";

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

/* --- kick sample: flash -> RAM ------------------------------------------- */

static uint8_t *kick_record;   /* sample_header + PCM, malloc'd */
static uint32_t kick_frames;
static int kick_state;         /* 0 none, 1 loaded, <0 error */

static void kick_unload(void)
{
    if (!kick_record) return;
    audio_set_sample(NULL, 0);
    free(kick_record);
    kick_record = NULL;
    kick_frames = 0;
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
        free(record); kick_state = -2; return;
    }
    struct sample_header h;
    memcpy(&h, record, sizeof h);
    if (h.magic != SAMPLE_MAGIC || (uint64_t)h.frames * 2 + sizeof h != got) {
        free(record); kick_state = -3; return;
    }
    kick_record = record;
    kick_frames = h.frames;
    kick_state = 1;
    audio_set_sample((const int16_t *)(const void *)(record + sizeof h), h.frames);
}

/* Voice 1 plays the uploaded kick; without one it falls back to the FM kick. */
static void apply_kit(void)
{
    if (kit.voice[0].engine == DRUM_SAMPLE && kick_state != 1)
        drum_voice_fm_kick(&kit.voice[0]);
    drums_prepare(&drums, &kit);
}

/* --- project ------------------------------------------------------------- */

/* Saving while the beat plays: the runtime silences audio during every
 * flash erase and write, so a save takes a snapshot and writes it in small
 * chunks, one per control tick, into space store_prepare() erased earlier
 * (while stopped). The store's commit rules still apply: the old project
 * stays valid until the new header is written. */
#define SAVE_CHUNK 256u
static struct project_file save_buf;
static uint32_t save_at;
static int saving;

int project_save(void)
{
    if (rescue || store_state != 0) return STORE_READ_ONLY;
    if (saving || store.stream.active) return STORE_INVALID; /* busy */
    save_buf.magic = PROJECT_MAGIC;
    save_buf.version = PROJECT_VERSION;
    save_buf.bytes = (uint16_t)sizeof save_buf;
    save_buf.pattern = pattern;
    save_buf.kit = kit;
    int r = store_begin(&store, PROJECT_KEY, sizeof save_buf);
    if (r) return r;
    save_at = 0;
    saving = 1;
    return STORE_OK;
}

static void save_poll(void)
{
    if (!saving) return;
    uint32_t left = sizeof save_buf - save_at;
    uint32_t n = left < SAVE_CHUNK ? left : SAVE_CHUNK;
    int r = store_append(&store, (const uint8_t *)&save_buf + save_at, n);
    if (r == STORE_OK) {
        save_at += n;
        if (save_at < sizeof save_buf) return;
        r = store_commit(&store, store_crc32(&save_buf, sizeof save_buf, 0));
    }
    saving = 0;
    drum_ui_say(r == STORE_OK ? "SAVED" : "SAVE FAILED");
}

static void project_load(void)
{
    static struct project_file f;
    size_t got;
    if (store_state == 0 &&
        store_read(&store, PROJECT_KEY, &f, sizeof f, &got) == STORE_OK &&
        got == sizeof f && f.magic == PROJECT_MAGIC && f.version == PROJECT_VERSION &&
        f.bytes == sizeof f) {
        pattern = f.pattern;
        kit = f.kit;
        return;
    }
    /* First boot: the default kit and a starter beat. */
    seq_pattern_init(&pattern);
    drum_kit_default(&kit);
    static const char *const starter[4] = {
        "x...x...x..x.x..", "....x.......x...", "x.x.x.x.x.x.x.x.", "......x.......x.",
    };
    for (unsigned v = 0; v < 4; v++)
        for (unsigned s = 0; s < 16; s++)
            pattern.voices[v].steps[s].on = starter[v][s] == 'x';
}

/* --- free RAM probe ------------------------------------------------------ */

static uint32_t probe_heap(void)
{
    uint32_t lo = 0, hi = 320;       /* KB; more than the heap */
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        void *p = malloc(mid * 1024u);
        if (p) { free(p); lo = mid; } else { hi = mid - 1; }
    }
    return lo * 1024u;
}

/* --- status -------------------------------------------------------------- */

const char *status_text(void)
{
    static char t[40];
    char *p = t;
    if (rescue) { fmt_str(t, "RESCUE: FLASH UNTOUCHED"); return t; }
    if (store_state < 0) { fmt_str(t, "FLASH ERROR"); return t; }
    p = fmt_str(p, kick_state == 1 ? "KICK OK  " : "NO KICK  ");
    fmt_str(p, upload_text);
    return t;
}

static void draw_status_page(void)
{
    char t[40], *p;
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_text(8, 6, "ERISIAN", 3, COLOR_GOLD, COLOR_BG);
    fmt_str(fmt_str(t, "V "), ERISIAN_VERSION);
    draw_line(8, 32, t, 1, COLOR_DIM, COLOR_BG);

    p = fmt_str(t, "RAM ");
    if (largest_free) { p = fmt_uint(p, largest_free / 1024); fmt_str(p, "K FREE"); } else fmt_str(p, "...");
    draw_line(8, 60, t, 2, COLOR_TEXT, COLOR_BG);
    if (rescue) {
        draw_line(8, 84, "RESCUE MODE", 2, COLOR_RED, COLOR_BG);
    } else if (store_state < 0) {
        fmt_uint(fmt_str(t, "FLASH ERR "), (uint32_t)-store_state);
        draw_line(8, 84, t, 2, COLOR_RED, COLOR_BG);
    } else if (store_state == 0) {
        p = fmt_str(t, store.read_only ? "FLASH RO " : "FLASH OK ");
        fmt_str(fmt_uint(p, store_free_sectors(&store) * 4), "K");
        draw_line(8, 84, t, 2, COLOR_TEXT, COLOR_BG);
    }
    if (kick_state == 1) {
        fmt_str(fmt_uint(fmt_str(t, "KICK "), kick_frames), " FR");
        draw_line(8, 108, t, 2, COLOR_GOLD, COLOR_BG);
    } else {
        draw_line(8, 108, kick_state ? "KICK ERROR" : "KICK NONE", 2, COLOR_DIM, COLOR_BG);
    }
    draw_line(8, 132, upload_text, 2, COLOR_TEXT, COLOR_BG);
    draw_line(8, 200, "HOME: BACK TO DRUMS", 1, COLOR_DIM, COLOR_BG);
    draw_line(8, 212, "UPLOAD KICK: ERISIAN-UPLOADER.HTML", 1, COLOR_DIM, COLOR_BG);
}

/* --- upload over SysEx --------------------------------------------------- */

static void handle_upload(void)
{
    if (reply_bytes) {
        if (fm1_midi_send_port(reply_port, reply, reply_bytes) != -2)
            reply_bytes = 0;  /* sent, or the port went away */
        return;
    }
    size_t n = mailbox_bytes;
    if (!n) return;
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
        if (kick_state == 1 && kit.voice[0].engine != DRUM_SAMPLE) {
            kit.voice[0].engine = DRUM_SAMPLE;   /* a fresh kick goes on voice 1 */
            kit.voice[0].p[DP_TUNE] = 64;
            kit.voice[0].p[DP_DECAY] = 127;
        }
        apply_kit();
        fmt_str(upload_text, kick_state == 1 ? "KICK SAVED" : "UPLOAD FAILED");
    } else if (cmd == UPLOAD_BEGIN) {
        fmt_str(upload_text, store.stream.active ? "USB RECEIVING" : "USB REFUSED");
        kick_unload();   /* RAM for the new one; the flash copy stays */
        apply_kit();
    } else if (cmd == UPLOAD_HELLO) {
        fmt_str(upload_text, "USB UPLOADER");
    }
    if (status_page) draw_status_page();
}

/* --- hooks --------------------------------------------------------------- */

void groove_screen_init(void)
{
    fm1_controls_init();
    fm1_screen_init();
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_text(8, 90, "ERISIAN", 3, COLOR_GOLD, COLOR_BG);
    char t[32];
    fmt_str(fmt_str(t, "V "), ERISIAN_VERSION);
    draw_line(8, 120, t, 1, COLOR_DIM, COLOR_BG);
    drums_init(&drums);
    seq_pattern_init(&pattern);
    drum_kit_default(&kit);
    apply_kit();
}

void groove_screen_control(void)
{
    static uint32_t start, last_poll, probe_at;
    static int started, booted, home_was;
    const uint32_t now = fm1_millis();
    if (!started) { started = 1; start = now; probe_at = now + 3000u; }

    handle_upload();
    if (now - last_poll < 10u) return;
    last_poll = now;
    save_poll();

    /* Erase free flash in the background, only while the beat is stopped
     * (an erase pauses audio), so saves during playback never erase. */
    static uint32_t last_prepare;
    if (booted && !rescue && store_state == 0 && !saving && !store.stream.active &&
        !audio_playing() && now - last_prepare >= 50u) {
        last_prepare = now;
        store_prepare(&store);
    }

    /* Boot: rescue check first (OCT- held), then mount flash and load. */
    if (!booted) {
        struct fm1_controls c;
        fm1_controls_read(&c);
        if (c.buttons >> BUTTON_OCT_DOWN & 1) rescue = 1;
        if (!rescue && now - start < RESCUE_WINDOW_MS) return;
        booted = 1;
        if (!rescue) {
            uint32_t size = fm1_flash_size();
            if (size > ERISIAN_FLASH_BYTES) size = ERISIAN_FLASH_BYTES;
            flash_dev.size = size / STORE_SECTOR_BYTES * STORE_SECTOR_BYTES;
            int r = store_mount(&store, &flash_dev, 0);
            store_state = r == STORE_OK ? 0 : (r < 0 ? r : -99);
            if (store_state == 0) {
                upload_init(&upload, &store);
                kick_load();
            }
        }
        project_load();
        apply_kit();
        status_page = rescue;
        if (status_page) draw_status_page(); else drum_ui_init();
        return;
    }

    if (!largest_free && (int32_t)(now - probe_at) >= 0) {
        largest_free = probe_heap();
        if (status_page) draw_status_page();
    }

    /* HOME toggles the status page. */
    struct fm1_controls peek;
    if (status_page) {
        fm1_controls_read(&peek);
        audio_set_master(peek.knob);
        int home = peek.buttons >> BUTTON_HOME & 1;
        if (home && !home_was) { status_page = 0; drum_ui_redraw(); }
        home_was = home;
        return;
    }
    /* The drum UI owns the panel read (a read consumes encoder movement). */
    if (drum_ui_poll(now)) {
        status_page = 1;
        home_was = 1;
        draw_status_page();
    }
}
