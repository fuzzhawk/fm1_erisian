/* SPDX-License-Identifier: MIT */
#include "store.h"

#include <string.h>

#define MAGIC 0x31535245u /* "ERS1" little-endian */
#define FLAG_TOMBSTONE 1u

struct header {
    uint32_t magic;
    uint16_t key;
    uint16_t flags;
    uint32_t seq;
    uint32_t length;
    uint32_t data_crc;
    uint32_t reserved[2];
    uint32_t header_crc; /* over the first 28 bytes */
};

uint32_t store_crc32(const void *data, size_t bytes, uint32_t crc)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (bytes--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static unsigned sectors_for(uint32_t length)
{
    return (unsigned)((STORE_HEADER_BYTES + length + STORE_SECTOR_BYTES - 1)
                      / STORE_SECTOR_BYTES);
}

static uint32_t sector_offset(unsigned sector)
{
    return (uint32_t)sector * STORE_SECTOR_BYTES;
}

/* CRC of a record's payload, read back from flash in small chunks. */
static int payload_crc(struct store *store, unsigned first, uint32_t length,
                       uint32_t *crc)
{
    uint8_t chunk[256];
    uint32_t offset = sector_offset(first) + STORE_HEADER_BYTES;
    uint32_t value = 0;
    while (length) {
        size_t n = length < sizeof chunk ? length : sizeof chunk;
        if (store->flash->read(store->flash->context, offset, chunk, n))
            return STORE_IO;
        value = store_crc32(chunk, n, value);
        offset += (uint32_t)n;
        length -= (uint32_t)n;
    }
    *crc = value;
    return STORE_OK;
}

static int read_header(struct store *store, unsigned sector, struct header *h)
{
    if (store->flash->read(store->flash->context, sector_offset(sector), h,
                           sizeof *h))
        return STORE_IO;
    if (h->magic != MAGIC)
        return STORE_INVALID;
    if (store_crc32(h, offsetof(struct header, header_crc), 0) != h->header_crc)
        return STORE_INVALID;
    if (h->key >= STORE_MAX_KEYS)
        return STORE_INVALID;
    if (sector + sectors_for(h->length) > store->sector_count)
        return STORE_INVALID;
    return STORE_OK;
}

static void mark(struct store *store, const struct store_entry *e, uint8_t value)
{
    for (unsigned i = 0; i < e->sectors; i++)
        store->live[e->first_sector + i] = value;
}

int store_mount(struct store *store, const struct store_flash *flash, int read_only)
{
    if (!flash || flash->size % STORE_SECTOR_BYTES ||
        flash->size / STORE_SECTOR_BYTES > STORE_MAX_SECTORS ||
        flash->size < 2 * STORE_SECTOR_BYTES)
        return STORE_INVALID;

    memset(store, 0, sizeof *store);
    store->flash = flash;
    store->sector_count = flash->size / STORE_SECTOR_BYTES;
    store->read_only = read_only;

    uint32_t newest_seq = 0;
    unsigned newest_end = 0;
    for (unsigned sector = 0; sector < store->sector_count; sector++) {
        struct header h;
        if (read_header(store, sector, &h) != STORE_OK)
            continue;
        uint32_t crc;
        int r = payload_crc(store, sector, h.length, &crc);
        if (r == STORE_IO)
            return r;
        if (crc != h.data_crc)
            continue;
        struct store_entry *e = &store->entries[h.key];
        if (e->sectors == 0 || h.seq > e->seq) {
            e->seq = h.seq;
            e->first_sector = (uint16_t)sector;
            e->sectors = (uint16_t)sectors_for(h.length);
            e->length = (h.flags & FLAG_TOMBSTONE) ? UINT32_MAX : h.length;
        }
        if (h.seq >= newest_seq) {
            newest_seq = h.seq;
            newest_end = sector + sectors_for(h.length);
        }
        /* A valid record owns its sectors: payload bytes are never headers. */
        sector += sectors_for(h.length) - 1;
    }
    for (unsigned key = 0; key < STORE_MAX_KEYS; key++)
        mark(store, &store->entries[key], 1);
    store->next_seq = newest_seq + 1;
    store->cursor = newest_end % store->sector_count;
    return STORE_OK;
}

int store_read(struct store *store, uint16_t key, void *data, size_t capacity,
               size_t *length)
{
    if (key >= STORE_MAX_KEYS)
        return STORE_INVALID;
    const struct store_entry *e = &store->entries[key];
    if (e->sectors == 0 || e->length == UINT32_MAX)
        return STORE_NOT_FOUND;
    if (length)
        *length = e->length;
    if (e->length > capacity)
        return STORE_TOO_SMALL;
    if (store->flash->read(store->flash->context,
                           sector_offset(e->first_sector) + STORE_HEADER_BYTES,
                           data, e->length))
        return STORE_IO;
    struct header h;
    if (read_header(store, e->first_sector, &h) != STORE_OK ||
        store_crc32(data, e->length, 0) != h.data_crc)
        return STORE_IO;
    return STORE_OK;
}

/* First run of n non-live sectors, searching from the cursor. */
static int allocate(struct store *store, unsigned n, unsigned *first)
{
    for (unsigned tried = 0; tried < store->sector_count; tried++) {
        unsigned start = (store->cursor + tried) % store->sector_count;
        if (start + n > store->sector_count)
            continue;
        unsigned i = 0;
        while (i < n && !store->live[start + i])
            i++;
        if (i == n) {
            *first = start;
            return STORE_OK;
        }
    }
    return STORE_FULL;
}

static void adopt(struct store *store, uint16_t key, uint16_t flags,
                  unsigned first, unsigned n, uint32_t seq, uint32_t length)
{
    struct store_entry *e = &store->entries[key];
    mark(store, e, 0);
    e->seq = seq;
    e->first_sector = (uint16_t)first;
    e->sectors = (uint16_t)n;
    e->length = (flags & FLAG_TOMBSTONE) ? UINT32_MAX : length;
    mark(store, e, 1);
    store->next_seq++;
    store->cursor = (first + n) % store->sector_count;
}

static int write_header(struct store *store, unsigned first, uint16_t key,
                        uint16_t flags, uint32_t length, uint32_t data_crc)
{
    struct header h;
    memset(&h, 0xFF, sizeof h);
    h.magic = MAGIC;
    h.key = key;
    h.flags = flags;
    h.seq = store->next_seq;
    h.length = length;
    h.data_crc = data_crc;
    uint32_t check;
    if (payload_crc(store, first, length, &check) || check != data_crc)
        return STORE_IO;
    h.header_crc = store_crc32(&h, offsetof(struct header, header_crc), 0);
    if (store->flash->write(store->flash->context, sector_offset(first), &h,
                            sizeof h)) /* commit */
        return STORE_IO;
    return STORE_OK;
}

static int write_record(struct store *store, uint16_t key, uint16_t flags,
                        const void *data, size_t length)
{
    if (store->read_only)
        return STORE_READ_ONLY;
    if (store->stream.active)
        return STORE_INVALID;
    if (key >= STORE_MAX_KEYS || (length && !data) ||
        length > (size_t)store->sector_count * STORE_SECTOR_BYTES)
        return STORE_INVALID;

    unsigned n = sectors_for((uint32_t)length), first;
    if (allocate(store, n, &first) != STORE_OK)
        return STORE_FULL;

    const struct store_flash *f = store->flash;
    for (unsigned i = 0; i < n; i++)
        if (f->erase(f->context, sector_offset(first + i)))
            return STORE_IO;
    if (length &&
        f->write(f->context, sector_offset(first) + STORE_HEADER_BYTES, data, length))
        return STORE_IO;

    uint32_t seq = store->next_seq;
    int r = write_header(store, first, key, flags, (uint32_t)length,
                         store_crc32(data, length, 0));
    if (r)
        return r;
    adopt(store, key, flags, first, n, seq, (uint32_t)length);
    return STORE_OK;
}

int store_write(struct store *store, uint16_t key, const void *data, size_t length)
{
    return write_record(store, key, 0, data, length);
}

int store_delete(struct store *store, uint16_t key)
{
    if (key < STORE_MAX_KEYS && store->entries[key].sectors == 0)
        return STORE_OK;
    return write_record(store, key, FLAG_TOMBSTONE, NULL, 0);
}

unsigned store_free_sectors(const struct store *store)
{
    unsigned free_sectors = 0;
    for (unsigned i = 0; i < store->sector_count; i++)
        free_sectors += !store->live[i];
    return free_sectors;
}

int store_begin(struct store *store, uint16_t key, uint32_t length)
{
    if (store->read_only)
        return STORE_READ_ONLY;
    if (store->stream.active || key >= STORE_MAX_KEYS || length == 0 ||
        length > (uint32_t)store->sector_count * STORE_SECTOR_BYTES)
        return STORE_INVALID;
    unsigned n = sectors_for(length), first;
    if (allocate(store, n, &first) != STORE_OK)
        return STORE_FULL;
    const struct store_flash *f = store->flash;
    for (unsigned i = 0; i < n; i++)
        if (f->erase(f->context, sector_offset(first + i)))
            return STORE_IO;
    store->stream.key = key;
    store->stream.first_sector = (uint16_t)first;
    store->stream.sectors = (uint16_t)n;
    store->stream.length = length;
    store->stream.written = 0;
    store->stream.crc = 0;
    store->stream.active = 1;
    return STORE_OK;
}

int store_append(struct store *store, const void *data, size_t bytes)
{
    struct store_stream *s = &store->stream;
    if (!s->active || (bytes && !data) || bytes > s->length - s->written)
        return STORE_INVALID;
    if (bytes && store->flash->write(store->flash->context,
                                     sector_offset(s->first_sector) +
                                         STORE_HEADER_BYTES + s->written,
                                     data, bytes)) {
        store_abort(store);
        return STORE_IO;
    }
    s->crc = store_crc32(data, bytes, s->crc);
    s->written += (uint32_t)bytes;
    return STORE_OK;
}

int store_commit(struct store *store, uint32_t expected_crc)
{
    struct store_stream *s = &store->stream;
    if (!s->active)
        return STORE_INVALID;
    if (s->written != s->length || s->crc != expected_crc) {
        store_abort(store);
        return STORE_INVALID;
    }
    uint32_t seq = store->next_seq;
    int r = write_header(store, s->first_sector, s->key, 0, s->length, s->crc);
    s->active = 0;
    if (r)
        return r;
    adopt(store, s->key, 0, s->first_sector, s->sectors, seq, s->length);
    return STORE_OK;
}

void store_abort(struct store *store)
{
    /* Nothing was committed: the reserved sectors are not live and get
     * reused by later writes. */
    store->stream.active = 0;
}

int store_locate(const struct store *store, uint16_t key, uint32_t *offset,
                 uint32_t *length)
{
    if (key >= STORE_MAX_KEYS)
        return STORE_INVALID;
    const struct store_entry *e = &store->entries[key];
    if (e->sectors == 0 || e->length == UINT32_MAX)
        return STORE_NOT_FOUND;
    if (offset)
        *offset = sector_offset(e->first_sector) + STORE_HEADER_BYTES;
    if (length)
        *length = e->length;
    return STORE_OK;
}
