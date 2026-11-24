/* SPDX-License-Identifier: MIT */
/* ERISIAN log-structured preset store.
 *
 * Records live in a region of 4 KB NOR sectors. Every save writes a new
 * record (key, sequence number, length, CRCs) into sectors that hold no live
 * data; the header is written last and acts as the commit. On mount the newest
 * valid record per key wins. A live record is never erased or overwritten, so
 * a power cut at any point leaves either the old or the new copy readable.
 *
 * The store is platform-independent: flash access goes through store_flash,
 * implemented by the FM-1 platform (fm1_flash_*) or by a host simulator.
 */
#ifndef ERISIAN_STORE_H
#define ERISIAN_STORE_H

#include <stddef.h>
#include <stdint.h>

#define STORE_SECTOR_BYTES 4096u
#define STORE_HEADER_BYTES 32u
#define STORE_MAX_SECTORS 160u   /* 640 KB; the FM-1 user partition is 524 KB */
#define STORE_MAX_KEYS 256u

enum store_result {
    STORE_OK = 0,
    STORE_NOT_FOUND = -1,
    STORE_FULL = -2,
    STORE_IO = -3,
    STORE_INVALID = -4,
    STORE_READ_ONLY = -5,
    STORE_TOO_SMALL = -6
};

struct store_flash {
    void *context;
    uint32_t size; /* bytes, multiple of STORE_SECTOR_BYTES */
    int (*read)(void *context, uint32_t offset, void *data, size_t bytes);
    int (*write)(void *context, uint32_t offset, const void *data, size_t bytes);
    int (*erase)(void *context, uint32_t offset); /* one aligned sector */
};

struct store_entry {
    uint32_t seq;
    uint16_t first_sector;
    uint16_t sectors;     /* 0 = no live record */
    uint32_t length;
};

/* An in-progress streamed write (store_begin / store_append / store_commit). */
struct store_stream {
    uint16_t key;
    uint16_t first_sector;
    uint16_t sectors;
    uint8_t active;
    uint32_t length;   /* announced total */
    uint32_t written;  /* appended so far */
    uint32_t crc;      /* running CRC-32 of appended bytes */
};

struct store {
    const struct store_flash *flash;
    unsigned sector_count;
    unsigned cursor;          /* next sector to try, spreads wear */
    uint32_t next_seq;
    int read_only;            /* set when the flash chip is not recognised */
    uint8_t live[STORE_MAX_SECTORS];
    uint8_t blank[STORE_MAX_SECTORS];   /* 0 unknown, 1 erased, 2 written */
    struct store_entry entries[STORE_MAX_KEYS];
    struct store_stream stream;
};

/* Scan the region and rebuild the index. read_only refuses all writes. */
int store_mount(struct store *store, const struct store_flash *flash, int read_only);

/* Copy the newest valid record for key into data (capacity bytes).
 * *length receives the record length. STORE_TOO_SMALL if it doesn't fit. */
int store_read(struct store *store, uint16_t key, void *data, size_t capacity,
               size_t *length);

int store_write(struct store *store, uint16_t key, const void *data, size_t length);

/* Streamed write of a large record (e.g. an 80 KB sample arriving over
 * SysEx) without holding it in RAM. store_begin reserves and erases space for
 * length bytes; store_append adds bytes in order; store_commit checks the total
 * and CRC and writes the header, which makes the new record live. Until then
 * the old record for key stays the valid one; a power cut or store_abort leaves
 * it untouched. One stream at a time; other writes are refused while open. */
int store_begin(struct store *store, uint16_t key, uint32_t length);
int store_append(struct store *store, const void *data, size_t bytes);
int store_commit(struct store *store, uint32_t expected_crc);
void store_abort(struct store *store);

/* Where a live record's payload sits in flash, for reading it in pieces
 * (e.g. loading a sample into RAM). */
int store_locate(const struct store *store, uint16_t key, uint32_t *offset,
                 uint32_t *length);

/* Background housekeeping: checks or erases ONE free sector so later saves
 * find erased space and need no erase (the FM-1 silences audio while it
 * erases). Returns 1 if it did something, 0 when every free sector is
 * already erased. Never touches live data; does nothing while a stream is
 * open. Call it when a pause in the audio does not matter (transport
 * stopped). */
int store_prepare(struct store *store);

/* Free sectors already known to be erased. */
unsigned store_erased_sectors(const struct store *store);

/* Writes an empty tombstone record; store_read then returns STORE_NOT_FOUND. */
int store_delete(struct store *store, uint16_t key);

/* Sectors not holding live records. */
unsigned store_free_sectors(const struct store *store);

uint32_t store_crc32(const void *data, size_t bytes, uint32_t crc);

#endif
