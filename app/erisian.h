/* SPDX-License-Identifier: MIT */
/* ERISIAN firmware: state shared between the control task and the audio
 * interrupt.
 *
 * Ownership:
 *  - the control task edits `pattern` and `kit` (byte-sized fields; a block
 *    that sees half an edit only plays one odd step), calls
 *    drums_prepare_voice() after kit edits, and sends transport and pad
 *    requests through the small queues below;
 *  - the audio interrupt owns `seq` and the drum voices, and reads the rest.
 */
#ifndef ERISIAN_H
#define ERISIAN_H

#include <stdint.h>

#include "engine/drums.h"
#include "engine/seq.h"
#include "engine/synth.h"

#ifndef ERISIAN_VERSION
#define ERISIAN_VERSION "dev"
#endif

extern struct seq_pattern pattern;
extern struct drum_kit kit;
extern struct drums drums;
extern struct seq_state seq;
extern struct synth_patch patch;     /* keys: the three layers */
extern struct synth synth;

/* --- audio.c: requests from the control task ----------------------------- */
void audio_play(void);
void audio_stop(void);
int audio_playing(void);
void audio_pad(unsigned voice, unsigned velocity);   /* live pad hit */
void audio_set_master(uint16_t knob);                 /* 0..1023 */
void audio_set_sample(const int16_t *pcm, uint32_t frames);
void audio_wait_blocks(unsigned blocks);              /* let the interrupt move on */
void audio_note(unsigned key, unsigned velocity);     /* keys: velocity 0 = off */
void audio_keys_off(void);                            /* release every keys voice */

/* What a UI poll asks main.c to do. */
enum { UI_STAY, UI_HOME, UI_SWITCH };

/* --- ui/drum_ui.c -------------------------------------------------------- */
void drum_ui_init(void);
void drum_ui_redraw(void);
/* Handle one panel scan. Returns UI_HOME (show status) or UI_SWITCH (SEQ:
 * to keys mode). */
int drum_ui_poll(uint32_t now_ms);
void drum_ui_say(const char *text);   /* short message in the header */

/* --- ui/keys_ui.c -------------------------------------------------------- */
void keys_ui_init(void);             /* state only; keys_ui_redraw draws */
void keys_ui_redraw(void);
int keys_ui_poll(uint32_t now_ms);    /* UI_HOME, or UI_SWITCH back to drums */
void keys_ui_say(const char *text);
void keys_ui_leave(void);             /* notes off before switching away */

/* --- main.c: services for the UI ----------------------------------------- */
/* Save the project. While playing it is written when the transport stops,
 * unless now != 0 (GLO + SAVE), which writes at once and measures it. */
int project_save(int now);
const char *status_text(void); /* short line for the header */

#endif
