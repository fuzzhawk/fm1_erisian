/* SPDX-License-Identifier: MIT */
#include "upload.h"

#include <string.h>

#include "../midi/sysex.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static size_t ack(uint8_t *reply, uint8_t cmd, uint8_t status, uint32_t value)
{
    uint8_t raw[6] = { cmd, status, (uint8_t)value, (uint8_t)(value >> 8),
                       (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    size_t n = 0;
    reply[n++] = 0xF0; reply[n++] = 0x7D; reply[n++] = 0x45; reply[n++] = 0x52;
    reply[n++] = UPLOAD_ACK;
    n += sysex_pack(raw, sizeof raw, reply + n);
    reply[n++] = 0xF7;
    return n; /* 5 + 7 + 1 = 13 */
}

static uint8_t flash_status(int r)
{
    return r == STORE_FULL ? UPLOAD_E_FULL
         : r == STORE_READ_ONLY || r == STORE_IO ? UPLOAD_E_FLASH
         : UPLOAD_E_ARGUMENT;
}

void upload_init(struct upload *upload, struct store *store)
{
    memset(upload, 0, sizeof *upload);
    upload->store = store;
}

int upload_handle(struct upload *u, const uint8_t *frame, size_t bytes,
                  uint8_t reply[UPLOAD_REPLY_MAX], size_t *reply_bytes)
{
    if (bytes < 6 || frame[0] != 0xF0 || frame[1] != 0x7D || frame[2] != 0x45 ||
        frame[3] != 0x52)
        return 0;
    uint8_t cmd = frame[4];
    if (frame[bytes - 1] != 0xF7) {
        *reply_bytes = ack(reply, cmd, UPLOAD_E_FRAME, 0);
        return 1;
    }
    uint8_t body[4 + UPLOAD_CHUNK_BYTES];
    size_t n = 0;
    if (bytes > 6) {
        n = sysex_unpack(frame + 5, bytes - 6, body, sizeof body);
        if (n == (size_t)-1) {
            *reply_bytes = ack(reply, cmd, UPLOAD_E_FRAME, 0);
            return 1;
        }
    }
    struct store_stream *s = &u->store->stream;
    uint8_t status = UPLOAD_OK;
    uint32_t value = 0;

    switch (cmd) {
    case UPLOAD_HELLO:
        value = UPLOAD_PROTOCOL;
        break;
    case UPLOAD_BEGIN: {
        if (n != 9) { status = UPLOAD_E_FRAME; break; }
        uint8_t slot = body[0];
        uint32_t length = get32(body + 1);
        if (slot >= UPLOAD_SLOTS || length <= sizeof(struct sample_header) ||
            length > UPLOAD_MAX_BYTES) { status = UPLOAD_E_ARGUMENT; break; }
        if (u->active)
            store_abort(u->store); /* a new BEGIN restarts */
        u->active = 0;
        int r = store_begin(u->store, UPLOAD_KEY(slot), length);
        if (r) { status = flash_status(r); break; }
        u->active = 1;
        u->slot = slot;
        u->expected_crc = get32(body + 5);
        break;
    }
    case UPLOAD_DATA: {
        if (!u->active) { status = UPLOAD_E_STATE; break; }
        if (n < 4) { status = UPLOAD_E_FRAME; break; }
        uint32_t offset = get32(body);
        size_t chunk = n - 4;
        if (offset < s->written && offset + chunk <= s->written) {
            value = s->written; /* duplicate: already have it */
            break;
        }
        if (offset != s->written) { status = UPLOAD_E_OFFSET; value = s->written; break; }
        if (offset == 0 && chunk >= sizeof(struct sample_header)) {
            struct sample_header h;
            memcpy(&h, body + 4, sizeof h);
            if (h.magic != SAMPLE_MAGIC || h.version != 1 || h.channels != 1 ||
                h.rate != 44100 ||
                (uint64_t)h.frames * 2 + sizeof h != s->length) {
                store_abort(u->store);
                u->active = 0;
                status = UPLOAD_E_ARGUMENT;
                break;
            }
        }
        int r = store_append(u->store, body + 4, chunk);
        if (r) { u->active = s->active; status = r == STORE_INVALID ? UPLOAD_E_ARGUMENT : UPLOAD_E_FLASH; break; }
        value = s->written;
        break;
    }
    case UPLOAD_END: {
        if (!u->active) { status = UPLOAD_E_STATE; break; }
        uint32_t length = s->length;
        int r = store_commit(u->store, u->expected_crc);
        u->active = 0;
        if (r) { status = r == STORE_INVALID ? UPLOAD_E_CRC : UPLOAD_E_FLASH; break; }
        value = length;
        break;
    }
    case UPLOAD_ABORT:
        if (u->active)
            store_abort(u->store);
        u->active = 0;
        break;
    default:
        status = UPLOAD_E_FRAME;
        break;
    }
    *reply_bytes = ack(reply, cmd, status, value);
    return 1;
}
