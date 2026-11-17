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

#ifndef ERISIAN_VERSION
#define ERISIAN_VERSION "dev"
#endif

extern struct seq_pattern pattern;
extern struct drum_kit kit;
extern struct drums drums;
extern struct seq_state seq;

/* --- audio.c: requests from the control task ----------------------------- */
void audio_play(void);
void audio_stop(void);
int audio_playing(void);
void audio_pad(unsigned voice, unsigned velocity);   /* live pad hit */
void audio_set_master(uint16_t knob);                 /* 0..1023 */
void audio_set_sample(const int16_t *pcm, uint32_t frames);
void audio_wait_blocks(unsigned blocks);              /* let the interrupt move on */

/* --- ui/drum_ui.c -------------------------------------------------------- */
void drum_ui_init(void);
void drum_ui_redraw(void);
/* Handle one panel scan. Returns 1 when HOME was pressed (show status). */
int drum_ui_poll(uint32_t now_ms);

/* --- main.c: services for the UI ----------------------------------------- */
int project_save(void);        /* 0 ok, else a store error */
const char *status_text(void); /* short line for the header */

#endif
