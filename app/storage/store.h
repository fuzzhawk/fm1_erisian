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

struct store {
    const struct store_flash *flash;
    unsigned sector_count;
    unsigned cursor;          /* next sector to try, spreads wear */
    uint32_t next_seq;
    int read_only;            /* set when the flash chip is not recognised */
    uint8_t live[STORE_MAX_SECTORS];
    struct store_entry entries[STORE_MAX_KEYS];
};

/* Scan the region and rebuild the index. read_only refuses all writes. */
int store_mount(struct store *store, const struct store_flash *flash, int read_only);

/* Copy the newest valid record for key into data (capacity bytes).
 * *length receives the record length. STORE_TOO_SMALL if it doesn't fit. */
int store_read(struct store *store, uint16_t key, void *data, size_t capacity,
               size_t *length);

int store_write(struct store *store, uint16_t key, const void *data, size_t length);

/* Writes an empty tombstone record; store_read then returns STORE_NOT_FOUND. */
int store_delete(struct store *store, uint16_t key);

/* Sectors not holding live records. */
unsigned store_free_sectors(const struct store *store);

uint32_t store_crc32(const void *data, size_t bytes, uint32_t crc);

#endif
