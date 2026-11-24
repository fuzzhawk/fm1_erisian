/* SPDX-License-Identifier: MIT */
/* MAVIS firmware: state shared between the control task and the audio
 * interrupt. The control task edits `patch` and `global` and calls
 * mv_prepare(); notes and the oshide press go through requests the audio
 * interrupt picks up at the start of each block. */
#ifndef MAVIS_H
#define MAVIS_H

#include <stdint.h>

#include "engine/engine.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

extern struct mavis_engine engine;
extern struct mv_patch patch;
extern struct mv_global global;

/* --- audio.c ------------------------------------------------------------- */
void audio_init(void);                              /* control task, once */
void audio_note(unsigned note, unsigned velocity);  /* velocity 0 = off */
void audio_all_off(void);
void audio_press(int on);                           /* oshide: press held strings */
void audio_set_master(uint16_t knob);               /* 0..1023 */
int audio_sounding(void);                           /* any voice active */
unsigned audio_voices(void);                        /* voices active */
/* CPU use of the audio interrupt, percent of the block time. */
unsigned audio_load(void);                          /* smoothed */
unsigned audio_load_peak(int reset);

/* --- ui/mavis_ui.c ------------------------------------------------------- */
enum { UI_STAY, UI_HOME };
void ui_init(void);
void ui_redraw(void);
int ui_poll(uint32_t now_ms);
void ui_say(const char *text);
void ui_visual(uint32_t now_ms);                   /* live visualiser frame */

/* --- main.c -------------------------------------------------------------- */
int sound_save(void);                               /* current sound + global */
const char *status_text(void);

#endif
