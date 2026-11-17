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
#include "engine/plock.h"
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
#define PROJECT_VERSION 5u   /* 5: keys patch (three layers) */

struct project_file {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    struct seq_pattern pattern;
    struct drum_kit kit;
    struct synth_patch patch;
};

/* Version 4: micro-timing and parameter locks, no keys patch. */
struct project_file_v4 {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    struct seq_pattern pattern;
    struct drum_kit kit;
};

/* Versions 2 and 3: the pattern before micro-timing and locks (3 stores FM
 * RATIO as 16 ranges of 0..127, 2 as an index). */
struct project_file_v3 {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    struct seq_pattern_v3 pattern;
    struct drum_kit kit;
};

/* Version 1 (before the engine refactor): old pattern, old kit layout. */
struct project_file_v1 {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    struct seq_pattern_v3 pattern;
    struct drum_kit_v1 kit;
};

/* --- flash and store ----------------------------------------------------- */

/* Flash access, counted and timed (see the save report on the status page). */
struct flash_stats;
static void flash_count(int kind, uint32_t ms);
static int flash_read(void *c, uint32_t o, void *d, size_t n)
{
    (void)c;
    uint32_t t = fm1_millis();
    int r = fm1_flash_read(o, d, n);
    flash_count(0, fm1_millis() - t);
    return r;
}
static int flash_write(void *c, uint32_t o, const void *d, size_t n)
{
    (void)c;
    uint32_t t = fm1_millis();
    int r = fm1_flash_write(o, d, n);
    flash_count(1, fm1_millis() - t);
    return r;
}
static int flash_erase(void *c, uint32_t o)
{
    (void)c;
    uint32_t t = fm1_millis();
    int r = fm1_flash_erase(o);
    flash_count(2, fm1_millis() - t);
    return r;
}

static struct store_flash flash_dev = { NULL, 0, flash_read, flash_write, flash_erase };
static struct store store;
static struct upload upload;
static int store_state = 1;    /* 1 = not mounted yet, 0 = ok, <0 = error */
static int rescue;
static int status_page;
static int keys_mode;            /* SEQ switches drum <-> keys */

static void ui_say(const char *text)
{
    if (keys_mode) keys_ui_say(text); else drum_ui_say(text);
}
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
    drums_set_tempo(&drums, &kit, pattern.bpm100);
    drums_prepare(&drums, &kit);
    plock_prepare_all(&pattern, &kit, &drums);
    synth_prepare(&synth, &patch);
}

/* --- project ------------------------------------------------------------- */

/* Saving while the beat plays. On hardware (2026-11-17) every flash erase
 * or write holds up audio rendering: blocks are skipped (buzz) and the
 * sequencer, which runs on rendered audio, hiccups. Splitting a save into
 * small writes made it choppier. So SAVE while playing takes a snapshot and
 * writes it in one go when the transport stops; GLO + SAVE forces an
 * immediate save and measures what it costs (status page). The store's
 * commit rules apply either way. */
static struct project_file save_buf;
static int save_pending;

struct flash_stats { uint32_t reads, writes, erases, max_ms; };
static struct flash_stats fstats;

struct save_report {
    uint32_t ms, writes, erases, max_ms, missed_blocks, overruns;
    int playing, result, valid;
};
static struct save_report last_save;

static void flash_count(int kind, uint32_t ms)
{
    if (kind == 0) fstats.reads++;
    else if (kind == 1) { fstats.writes++; if (ms > fstats.max_ms) fstats.max_ms = ms; }
    else { fstats.erases++; if (ms > fstats.max_ms) fstats.max_ms = ms; }
}

static void snapshot(void)
{
    save_buf.magic = PROJECT_MAGIC;
    save_buf.version = PROJECT_VERSION;
    save_buf.bytes = (uint16_t)sizeof save_buf;
    save_buf.pattern = pattern;
    save_buf.kit = kit;
    save_buf.patch = patch;
}

static int save_flush(void)
{
    struct fm1_audio_stats a0, a1;
    fstats.max_ms = 0;                 /* longest single operation in this save */
    struct flash_stats f0 = fstats;
    fm1_audio_stats_read(&a0);
    const uint32_t t0 = fm1_millis();
    int r = store_write(&store, PROJECT_KEY, &save_buf, sizeof save_buf);
    const uint32_t t1 = fm1_millis();
    fm1_audio_stats_read(&a1);
    save_pending = 0;

    /* Blocks the audio should have rendered in that time vs. what it did. */
    uint32_t ms = t1 - t0;
    uint32_t expected = (ms * 441u + 639u) / 1280u;   /* ms * 44100 / 128 / 1000 */
    uint32_t got = a1.blocks - a0.blocks;
    last_save.ms = ms;
    last_save.writes = fstats.writes - f0.writes;
    last_save.erases = fstats.erases - f0.erases;
    last_save.max_ms = fstats.max_ms;
    last_save.missed_blocks = expected > got ? expected - got : 0;
    last_save.overruns = a1.overruns - a0.overruns;
    last_save.playing = audio_playing();
    last_save.result = r;
    last_save.valid = 1;

    char t[32], *p = fmt_str(t, r == STORE_OK ? "SAVED " : "SAVE FAILED ");
    p = fmt_uint(p, ms);
    p = fmt_str(p, "MS -");
    p = fmt_uint(p, last_save.missed_blocks);
    fmt_str(p, "BLK");
    ui_say(t);
    return r;
}

int project_save(int now)
{
    if (rescue || store_state != 0) return STORE_READ_ONLY;
    if (store.stream.active) return STORE_INVALID;   /* an upload is running */
    snapshot();
    /* Measured on hardware: every flash write drops at least one whole
     * audio block (one 3.4 KB write: 8 blocks; 96 small writes: 96). So a
     * save while playing waits for STOP, unless forced with GLO + SAVE. */
    if (now || !audio_playing())
        return save_flush();
    save_pending = 1;
    ui_say("SAVE PENDING: STOP TO WRITE");
    return STORE_OK;
}

static void project_load(void)
{
    static union {
        struct project_file v5; struct project_file_v4 v4; struct project_file_v3 v3;
        struct project_file_v1 v1;
    } f;
    size_t got;
    synth_patch_default(&patch);                     /* older saves have no keys */
    if (store_state == 0 &&
        store_read(&store, PROJECT_KEY, &f, sizeof f, &got) == STORE_OK &&
        f.v5.magic == PROJECT_MAGIC) {
        if (f.v5.version == PROJECT_VERSION && got == sizeof f.v5 && f.v5.bytes == sizeof f.v5) {
            pattern = f.v5.pattern;
            kit = f.v5.kit;
            patch = f.v5.patch;
            plock_validate(&pattern);
            return;
        }
        if (f.v4.version == 4 && got == sizeof f.v4 && f.v4.bytes == sizeof f.v4) {
            pattern = f.v4.pattern;
            kit = f.v4.kit;
            plock_validate(&pattern);
            return;
        }
        if ((f.v3.version == 3 || f.v3.version == 2) && got == sizeof f.v3 &&
            f.v3.bytes == sizeof f.v3) {
            seq_pattern_from_v3(&pattern, &f.v3.pattern);
            kit = f.v3.kit;
            if (f.v3.version == 2)                   /* v2 stored FM RATIO as an index */
                for (unsigned v = 0; v < DRUM_VOICES; v++)
                    if (kit.voice[v].engine == DRUM_FM)
                        kit.voice[v].e[FM_RATIO] = (uint8_t)((kit.voice[v].e[FM_RATIO] & 15u) * 8u + 4u);
            return;
        }
        if (f.v1.version == 1 && got == sizeof f.v1 && f.v1.bytes == sizeof f.v1) {
            seq_pattern_from_v3(&pattern, &f.v1.pattern);   /* migrate both */
            drum_kit_from_v1(&kit, &f.v1.kit);
            return;
        }
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
    {   /* CPU headroom: overruns since boot (busy keys + beat is the test) */
        struct fm1_audio_stats as;
        fm1_audio_stats_read(&as);
        p = fmt_str(t, "AUDIO OVERRUNS ");
        p = fmt_uint(p, as.overruns);
        p = fmt_str(p, "  PEAK ");
        fmt_uint(p, as.peak_ticks);
        draw_line(8, 148, t, 1, COLOR_TEXT, COLOR_BG);
    }
    if (last_save.valid) {
        p = fmt_str(t, "LAST SAVE ");
        p = fmt_uint(p, last_save.ms);
        p = fmt_str(p, "MS W");
        p = fmt_uint(p, last_save.writes);
        p = fmt_str(p, " E");
        p = fmt_uint(p, last_save.erases);
        p = fmt_str(p, last_save.playing ? " PLAYING" : " STOPPED");
        draw_line(8, 160, t, 1, COLOR_TEXT, COLOR_BG);
        p = fmt_str(t, "AUDIO MISSED ");
        p = fmt_uint(p, last_save.missed_blocks);
        p = fmt_str(p, " BLK  OVR ");
        p = fmt_uint(p, last_save.overruns);
        p = fmt_str(p, "  MAX OP ");
        p = fmt_uint(p, last_save.max_ms);
        fmt_str(p, "MS");
        draw_line(8, 172, t, 1, COLOR_TEXT, COLOR_BG);
    }
    p = fmt_str(t, "ERASED FREE ");
    fmt_str(fmt_uint(p, store_state == 0 ? store_erased_sectors(&store) * 4 : 0), "K");
    draw_line(8, 184, t, 1, COLOR_DIM, COLOR_BG);
    draw_line(8, 200, "HOME: BACK", 1, COLOR_DIM, COLOR_BG);
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
            drum_voice_engine_defaults(&kit.voice[0], DRUM_SAMPLE);   /* fresh kick on voice 1 */
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
    synth_init(&synth);
    seq_pattern_init(&pattern);
    drum_kit_default(&kit);
    synth_patch_default(&patch);
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
    if (save_pending && !audio_playing())
        save_flush();   /* deferred save: the beat just stopped */

    /* Erase free flash in the background, only while the beat is stopped
     * (an erase pauses audio), so saves during playback never erase. */
    static uint32_t last_prepare;
    if (booted && !rescue && store_state == 0 && !save_pending && !store.stream.active &&
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
        keys_ui_init();
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
        if (home && !home_was) { status_page = 0; if (keys_mode) keys_ui_redraw(); else drum_ui_redraw(); }
        home_was = home;
        return;
    }
    /* The drum UI owns the panel read (a read consumes encoder movement). */
    int r = keys_mode ? keys_ui_poll(now) : drum_ui_poll(now);
    if (r == UI_SWITCH) {                       /* SEQ: drum <-> keys */
        if (keys_mode) { keys_ui_leave(); keys_mode = 0; drum_ui_redraw(); }
        else { keys_mode = 1; keys_ui_redraw(); }
    } else if (r == UI_HOME) {
        if (keys_mode) keys_ui_leave();
        status_page = 1;
        home_was = 1;
        draw_status_page();
    }
}
