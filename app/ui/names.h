/* SPDX-License-Identifier: MIT */
/* Discordian Markov names for patterns, kits and patches. */
#ifndef ERISIAN_NAMES_H
#define ERISIAN_NAMES_H

#include <stdint.h>

#define NAME_MAX_CHARS 12u

/* Characters a name may contain, in the order the character encoder steps
 * through them. */
extern const char name_charset[];

/* Generate a name from *seed (advanced in place). out gets 1..12 chars of
 * name_charset (upper case), NUL-terminated. */
void name_generate(uint32_t *seed, char out[NAME_MAX_CHARS + 1]);

/* Step the character at position by delta through name_charset (wrapping).
 * Positions past the end extend the name with spaces; trailing spaces are
 * trimmed. Returns the new length. */
unsigned name_edit(char name[NAME_MAX_CHARS + 1], unsigned position, int delta);

#endif
