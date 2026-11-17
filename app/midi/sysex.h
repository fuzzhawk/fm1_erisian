/* SPDX-License-Identifier: MIT */
/* 7-bit packing for SysEx payloads: every 7 bytes travel as 8, the first
 * carrying their top bits (bit 6 = byte 0's MSB ... bit 0 = byte 6's MSB). */
#ifndef ERISIAN_SYSEX_H
#define ERISIAN_SYSEX_H

#include <stddef.h>
#include <stdint.h>

#define SYSEX_PACKED_SIZE(n) ((n) + ((n) + 6) / 7)

/* Returns packed length. out must hold SYSEX_PACKED_SIZE(n) bytes. */
size_t sysex_pack(const uint8_t *in, size_t n, uint8_t *out);

/* Returns unpacked length, or (size_t)-1 if a byte has bit 7 set, the input
 * ends in a group with no data bytes, or the result exceeds capacity. */
size_t sysex_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t capacity);

#endif
