/* SPDX-License-Identifier: MIT */
/* MAVIS: boot, rescue check, flash store, the current sound, status page.
 * The panel and screens are in ui/mavis_ui.c; sound in engine/ and audio.c.
 *
 * Storage: the same log-structured store as ERISIAN (shared/storage), in the
 * same region, under MAVIS's own keys (0x40..) and magic, so both firmwares'
 * data live side by side. */
#include <stdlib.h>
#include <string.h>

#include "mavis.h"
#include "fm1.h"
#include "fm1_app.h"
#include "storage/store.h"
#include "ui/draw.h"

/* Same region as ERISIAN: below stock V15's patch bank (user offset 0x6E000). */
#define MAVIS_FLASH_BYTES 0x6E000u
#define RESCUE_WINDOW_MS 200u
#define BUTTON_OCT_DOWN 0u
#define BUTTON_HOME 8u

#define SOUND_KEY 0x41u
#define SOUND_MAGIC 0x3153564Du /* "MVS1" */
#define SOUND_VERSION 1u

struct sound_file {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    struct mv_patch patch;
    struct mv_global global;
};

static int flash_read(void *c, uint32_t o, void *d, size_t n) { (void)c; return fm1_flash_read(o, d, n); }
static int flash_write(void *c, uint32_t o, const void *d, size_t n) { (void)c; return fm1_flash_write(o, d, n); }
static int flash_erase(void *c, uint32_t o) { (void)c; return fm1_flash_erase(o); }

static struct store_flash flash_dev = { NULL, 0, flash_read, flash_write, flash_erase };
static struct store store;
static int store_state = 1;    /* 1 = not mounted yet, 0 = ok, <0 = error */
static int rescue;
static int status_page;
static int save_pending;
static uint32_t largest_free;

/* --- the current sound --------------------------------------------------- */

static void sound_apply(void)
{
    mv_prepare(&engine, &patch, &global);
}

static void sound_load(void)
{
    static struct sound_file f;
    size_t got;
    mv_patch_default(&patch, MV_KOTO);
    mv_global_default(&global);
    if (store_state == 0 && store_read(&store, SOUND_KEY, &f, sizeof f, &got) == STORE_OK &&
        f.magic == SOUND_MAGIC && f.version == SOUND_VERSION && got == sizeof f &&
        f.bytes == sizeof f) {
        patch = f.patch;
        global = f.global;
    }
}

static int sound_write(void)
{
    static struct sound_file f;
    f.magic = SOUND_MAGIC;
    f.version = SOUND_VERSION;
    f.bytes = (uint16_t)sizeof f;
    f.patch = patch;
    f.global = global;
    save_pending = 0;
    int r = store_write(&store, SOUND_KEY, &f, sizeof f);
    ui_say(r == STORE_OK ? "SAVED" : "SAVE FAILED");
    return r;
}

/* Every flash write drops audio, so a save waits until nothing sounds. */
int sound_save(void)
{
    if (rescue || store_state != 0) { ui_say("NO FLASH: NOT SAVED"); return STORE_READ_ONLY; }
    if (!audio_sounding()) return sound_write();
    save_pending = 1;
    ui_say("SAVE PENDING: RELEASE KEYS");
    return STORE_OK;
}

/* --- status --------------------------------------------------------------- */

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

const char *status_text(void)
{
    static char t[40];
    if (rescue) { fmt_str(t, "RESCUE: FLASH UNTOUCHED"); return t; }
    if (store_state < 0) { fmt_str(t, "FLASH ERROR: NO SAVES"); return t; }
    char *p = fmt_str(t, "CPU ");
    p = fmt_uint(p, audio_load());
    p = fmt_str(p, "%  VOICES ");
    p = fmt_uint(p, audio_voices());
    p = fmt_str(p, "/");
    fmt_uint(p, global.voices);
    return t;
}

static void draw_status_page(void)
{
    char t[40], *p;
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_text(8, 6, "MAVIS", 3, COLOR_RED, COLOR_BG);
    fmt_str(fmt_str(t, "V "), FW_VERSION);
    draw_line(8, 32, t, 1, COLOR_DIM, COLOR_BG);
    p = fmt_str(t, "RAM ");
    if (largest_free) { p = fmt_uint(p, largest_free / 1024); fmt_str(p, "K FREE"); } else fmt_str(p, "...");
    draw_line(8, 56, t, 2, COLOR_TEXT, COLOR_BG);
    if (rescue) {
        draw_line(8, 80, "RESCUE MODE", 2, COLOR_RED, COLOR_BG);
    } else if (store_state < 0) {
        fmt_uint(fmt_str(t, "FLASH ERR "), (uint32_t)-store_state);
        draw_line(8, 80, t, 2, COLOR_RED, COLOR_BG);
    } else if (store_state == 0) {
        p = fmt_str(t, store.read_only ? "FLASH RO " : "FLASH OK ");
        fmt_str(fmt_uint(p, store_free_sectors(&store) * 4), "K");
        draw_line(8, 80, t, 2, COLOR_TEXT, COLOR_BG);
    }
    p = fmt_str(t, "CPU ");
    p = fmt_uint(p, audio_load());
    p = fmt_str(p, "% PEAK ");
    p = fmt_uint(p, audio_load_peak(0));
    fmt_str(p, "%");
    draw_line(8, 104, t, 2, COLOR_TEXT, COLOR_BG);
    struct fm1_audio_stats as;
    fm1_audio_stats_read(&as);
    p = fmt_str(t, "OVERRUNS ");
    fmt_uint(p, as.overruns);
    draw_line(8, 128, t, 2, as.overruns ? COLOR_RED : COLOR_TEXT, COLOR_BG);
    p = fmt_str(t, "VOICES ");
    p = fmt_uint(p, audio_voices());
    p = fmt_str(p, " OF ");
    fmt_uint(p, global.voices);
    draw_line(8, 152, t, 2, COLOR_TEXT, COLOR_BG);
    draw_line(8, 188, "PLAY CHORDS, WATCH CPU PEAK", 1, COLOR_DIM, COLOR_BG);
    draw_line(8, 200, "GLO: RESET PEAK   HOME: BACK", 1, COLOR_DIM, COLOR_BG);
}

/* --- hooks ---------------------------------------------------------------- */

void groove_screen_init(void)
{
    fm1_controls_init();
    fm1_screen_init();
    fm1_screen_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, COLOR_BG);
    draw_text(8, 90, "MAVIS", 3, COLOR_RED, COLOR_BG);
    char t[32];
    fmt_str(fmt_str(t, "V "), FW_VERSION);
    draw_line(8, 120, t, 1, COLOR_DIM, COLOR_BG);
    mv_init(&engine);
    mv_patch_default(&patch, MV_KOTO);
    mv_global_default(&global);
    sound_apply();
    audio_init();
}

void groove_screen_control(void)
{
    static uint32_t start, last_poll, probe_at, last_status;
    static int started, booted, home_was;
    const uint32_t now = fm1_millis();
    if (!started) { started = 1; start = now; probe_at = now + 3000u; }
    if (now - last_poll < 10u) return;
    last_poll = now;

    /* Boot: rescue check first (OCT- held), then mount flash and load. */
    if (!booted) {
        struct fm1_controls c;
        fm1_controls_read(&c);
        if (c.buttons >> BUTTON_OCT_DOWN & 1) rescue = 1;
        if (!rescue && now - start < RESCUE_WINDOW_MS) return;
        booted = 1;
        if (!rescue) {
            uint32_t size = fm1_flash_size();
            if (size > MAVIS_FLASH_BYTES) size = MAVIS_FLASH_BYTES;
            flash_dev.size = size / STORE_SECTOR_BYTES * STORE_SECTOR_BYTES;
            int r = store_mount(&store, &flash_dev, 0);
            store_state = r == STORE_OK ? 0 : (r < 0 ? r : -99);
        }
        sound_load();
        sound_apply();
        ui_init();
        status_page = rescue;
        if (status_page) draw_status_page(); else ui_redraw();
        return;
    }

    /* Flash work only in silence: a write or erase drops audio blocks. The
     * background erase also waits 3 s, so it is not under the next note. */
    static uint32_t last_sound, last_prepare;
    if (audio_sounding()) last_sound = now;
    if (save_pending && !audio_sounding()) sound_write();
    if (!rescue && store_state == 0 && !save_pending && now - last_sound >= 3000u &&
        now - last_prepare >= 100u) {
        last_prepare = now;
        store_prepare(&store);
    }

    if (!largest_free && (int32_t)(now - probe_at) >= 0) largest_free = probe_heap();

    if (status_page) {
        struct fm1_controls c;
        fm1_controls_read(&c);
        audio_set_master(c.knob);
        int home = c.buttons >> BUTTON_HOME & 1;
        if (c.buttons >> 7 & 1) audio_load_peak(1);      /* GLO: reset the peak */
        if (home && !home_was) { status_page = 0; ui_redraw(); }
        home_was = home;
        if (status_page && now - last_status >= 250u) { last_status = now; draw_status_page(); }
        return;
    }
    if (ui_poll(now) == UI_HOME) {
        audio_all_off();
        status_page = 1;
        home_was = 1;
        draw_status_page();
        return;
    }
    ui_visual(now);
}
