/* SPDX-License-Identifier: MIT */
/* ERISIAN sample upload over SysEx (USB MIDI).
 *
 * Frame: F0 7D 45 52 <cmd> <7-bit packed payload> F7
 *   7D = non-commercial manufacturer ID, 45 52 = "ER".
 * Host -> FM-1:
 *   01 HELLO  -                                    -> ACK value = protocol version
 *   10 BEGIN  u8 slot, u32 length, u32 crc32        -> ACK value = 0 (space erased)
 *   11 DATA   u32 offset, up to 512 bytes           -> ACK value = next offset
 *   12 END    -                                     -> ACK value = length
 *   13 ABORT  -                                     -> ACK
 * FM-1 -> host:
 *   7F ACK    u8 cmd, u8 status, u32 value          (status 0 = ok)
 * Integers are little-endian. The payload stored is a 16-byte sample header
 * (struct sample_header) followed by 16-bit little-endian mono PCM. DATA
 * with an offset below the current one is acknowledged without writing, so a
 * host may resend after a lost reply. Writes go through the log-structured
 * store, so an interrupted upload leaves the previous sample in place.
 */
#ifndef ERISIAN_UPLOAD_H
#define ERISIAN_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "storage/store.h"

#define UPLOAD_PROTOCOL 1u
#define UPLOAD_CHUNK_BYTES 512u
#define UPLOAD_SLOTS 1u                 /* slot 0: the wav kick */
#define UPLOAD_MAX_BYTES (96u * 1024u)  /* header + PCM */
#define UPLOAD_KEY(slot) ((uint16_t)(0xF0u + (slot)))
#define UPLOAD_REPLY_MAX 16u

#define SAMPLE_MAGIC 0x4B535245u /* "ERSK" */

struct sample_header {
    uint32_t magic;
    uint16_t version;   /* 1 */
    uint16_t channels;  /* 1 */
    uint32_t rate;      /* 44100 */
    uint32_t frames;
};

enum upload_cmd {
    UPLOAD_HELLO = 0x01, UPLOAD_BEGIN = 0x10, UPLOAD_DATA = 0x11,
    UPLOAD_END = 0x12, UPLOAD_ABORT = 0x13, UPLOAD_ACK = 0x7F
};

enum upload_status {
    UPLOAD_OK = 0,
    UPLOAD_E_FRAME = 1,     /* not a valid ERISIAN frame */
    UPLOAD_E_STATE = 2,     /* command not expected now */
    UPLOAD_E_ARGUMENT = 3,  /* bad slot, length or header */
    UPLOAD_E_OFFSET = 4,    /* gap in DATA; value = expected offset */
    UPLOAD_E_FULL = 5,      /* not enough free flash */
    UPLOAD_E_FLASH = 6,     /* flash error or unknown chip (read-only) */
    UPLOAD_E_CRC = 7        /* END: length or CRC mismatch */
};

struct upload {
    struct store *store;
    uint32_t expected_crc;
    uint8_t active;
    uint8_t slot;
};

void upload_init(struct upload *upload, struct store *store);

/* Handle one complete SysEx frame (F0 .. F7). Returns 1 and fills reply
 * (a complete frame, *reply_bytes long) if the frame was ours, 0 if it was
 * someone else's SysEx. Control task only: it may erase and write flash. */
int upload_handle(struct upload *upload, const uint8_t *frame, size_t bytes,
                  uint8_t reply[UPLOAD_REPLY_MAX], size_t *reply_bytes);

#endif
