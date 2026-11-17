/* deps: app/storage/store.c */
/* Host tests for the log-structured store: NOR semantics, power cuts at every
 * write/erase step, bit flips, full flash, wear spreading. */
#include "../app/storage/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_BYTES (128u * STORE_SECTOR_BYTES) /* 512 KB */

static int failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); } } while (0)

/* --- NOR flash simulator with power-cut injection ----------------------- */

struct sim {
    uint8_t mem[FLASH_BYTES];
    unsigned erases[FLASH_BYTES / STORE_SECTOR_BYTES];
    long budget; /* byte writes + erases allowed before power is cut; <0 = no limit */
    int dead;
};

static int sim_spend(struct sim *s)
{
    if (s->dead) return 1;
    if (s->budget == 0) { s->dead = 1; return 1; }
    if (s->budget > 0) s->budget--;
    return 0;
}

static int sim_read(void *c, uint32_t off, void *d, size_t n)
{
    struct sim *s = c;
    if (off + n > FLASH_BYTES) return -1;
    memcpy(d, s->mem + off, n);
    return 0;
}

static int sim_write(void *c, uint32_t off, const void *d, size_t n)
{
    struct sim *s = c;
    const uint8_t *p = d;
    if (off + n > FLASH_BYTES) return -1;
    for (size_t i = 0; i < n; i++) {
        if (sim_spend(s)) return -2;
        if ((s->mem[off + i] & p[i]) != p[i]) return -4; /* needs erase */
        s->mem[off + i] &= p[i];
    }
    return 0;
}

static int sim_erase(void *c, uint32_t off)
{
    struct sim *s = c;
    if (off % STORE_SECTOR_BYTES || off >= FLASH_BYTES) return -1;
    if (sim_spend(s)) {
        /* A cut erase leaves the sector half-erased garbage. */
        for (unsigned i = 0; i < STORE_SECTOR_BYTES / 2; i++)
            s->mem[off + i] = 0xFF;
        return -2;
    }
    memset(s->mem + off, 0xFF, STORE_SECTOR_BYTES);
    s->erases[off / STORE_SECTOR_BYTES]++;
    return 0;
}

static struct sim sim;
static struct store_flash flash = { &sim, FLASH_BYTES, sim_read, sim_write, sim_erase };
static struct store st;

static void sim_reset(void)
{
    memset(&sim, 0, sizeof sim);
    memset(sim.mem, 0xFF, sizeof sim.mem);
    sim.budget = -1;
}

static void power_on(void)
{
    sim.dead = 0;
    sim.budget = -1;
    CHECK(store_mount(&st, &flash, 0) == STORE_OK);
}

static void fill(uint8_t *buf, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(seed * 131u + i * 7u + (i >> 8));
}

static int has(uint16_t key, const uint8_t *expect, size_t n)
{
    static uint8_t buf[96 * 1024];
    size_t len = 0;
    if (store_read(&st, key, buf, sizeof buf, &len) != STORE_OK) return 0;
    return len == n && memcmp(buf, expect, n) == 0;
}

/* --- tests --------------------------------------------------------------- */

static void test_roundtrip(void)
{
    uint8_t a[9000], b[100];
    sim_reset(); power_on();
    fill(a, sizeof a, 1); fill(b, sizeof b, 2);
    CHECK(store_write(&st, 3, a, sizeof a) == STORE_OK);
    CHECK(store_write(&st, 4, b, sizeof b) == STORE_OK);
    CHECK(has(3, a, sizeof a) && has(4, b, sizeof b));
    power_on(); /* remount */
    CHECK(has(3, a, sizeof a) && has(4, b, sizeof b));
    size_t len;
    CHECK(store_read(&st, 5, a, sizeof a, &len) == STORE_NOT_FOUND);
    CHECK(store_read(&st, 3, b, sizeof b, &len) == STORE_TOO_SMALL && len == sizeof a);
    CHECK(store_delete(&st, 4) == STORE_OK);
    power_on();
    CHECK(store_read(&st, 4, b, sizeof b, &len) == STORE_NOT_FOUND);
    CHECK(has(3, a, sizeof a));
}

static void test_overwrite_keeps_newest(void)
{
    uint8_t v[5000];
    sim_reset(); power_on();
    for (unsigned i = 0; i < 50; i++) {
        fill(v, sizeof v, i);
        CHECK(store_write(&st, 7, v, sizeof v) == STORE_OK);
    }
    power_on();
    fill(v, sizeof v, 49);
    CHECK(has(7, v, sizeof v));
    CHECK(store_free_sectors(&st) == 128 - 2);
}

/* Cut power at every possible step of one save; afterwards the key holds the
 * old or the new value exactly, and an unrelated key is untouched. */
static void test_power_cut_every_step(void)
{
    static uint8_t old_v[6000], new_v[6000], other[300];
    fill(old_v, sizeof old_v, 10); fill(new_v, sizeof new_v, 11);
    fill(other, sizeof other, 12);

    /* Count steps of an uninterrupted save. */
    sim_reset(); power_on();
    CHECK(store_write(&st, 1, old_v, sizeof old_v) == STORE_OK);
    CHECK(store_write(&st, 2, other, sizeof other) == STORE_OK);
    static uint8_t snapshot[FLASH_BYTES];
    memcpy(snapshot, sim.mem, FLASH_BYTES);
    sim.budget = 1L << 30;
    CHECK(store_write(&st, 1, new_v, sizeof new_v) == STORE_OK);
    long steps = (1L << 30) - sim.budget;

    unsigned saw_old = 0, saw_new = 0;
    for (long cut = 0; cut <= steps; cut++) {
        memcpy(sim.mem, snapshot, FLASH_BYTES);
        power_on();
        sim.budget = cut;
        (void)store_write(&st, 1, new_v, sizeof new_v);
        power_on();
        int o = has(1, old_v, sizeof old_v), n = has(1, new_v, sizeof new_v);
        CHECK(o || n);
        CHECK(has(2, other, sizeof other));
        saw_old += o; saw_new += n;
        if (!(o || n)) { fprintf(stderr, "  cut at step %ld/%ld\n", cut, steps); break; }
    }
    CHECK(saw_old > 0 && saw_new > 0);
}

static void test_bit_flip_falls_back(void)
{
    uint8_t a[2000], b[2000];
    sim_reset(); power_on();
    fill(a, sizeof a, 20); fill(b, sizeof b, 21);
    CHECK(store_write(&st, 9, a, sizeof a) == STORE_OK);
    unsigned old_sector = st.entries[9].first_sector;
    CHECK(store_write(&st, 9, b, sizeof b) == STORE_OK);
    unsigned new_sector = st.entries[9].first_sector;
    CHECK(old_sector != new_sector);
    sim.mem[new_sector * STORE_SECTOR_BYTES + STORE_HEADER_BYTES + 500] ^= 0x10;
    power_on();
    CHECK(has(9, a, sizeof a)); /* corrupted newest copy ignored, older wins */
    sim.mem[old_sector * STORE_SECTOR_BYTES + 3] ^= 0x01; /* header of old */
    power_on();
    size_t len;
    CHECK(store_read(&st, 9, a, sizeof a, &len) == STORE_NOT_FOUND);
}

static void test_full_and_read_only(void)
{
    static uint8_t big[3 * STORE_SECTOR_BYTES - STORE_HEADER_BYTES]; /* exactly 3 sectors */
    sim_reset(); power_on();
    unsigned written = 0;
    for (uint16_t key = 0; key < 64; key++) {
        fill(big, sizeof big, key);
        if (store_write(&st, key, big, sizeof big) != STORE_OK) break;
        written++;
    }
    CHECK(written == 42); /* 42 * 3 = 126 of 128 sectors */
    CHECK(store_write(&st, 100, big, sizeof big) == STORE_FULL);
    fill(big, sizeof big, 5);
    CHECK(has(5, big, sizeof big)); /* failed save destroyed nothing */
    /* Overwriting an existing key still works while 2 sectors are spare? No:
     * it needs 3 free sectors, so this must fail safely too. */
    CHECK(store_write(&st, 5, big, sizeof big) == STORE_FULL);
    CHECK(has(5, big, sizeof big));
    CHECK(store_mount(&st, &flash, 1) == STORE_OK);
    CHECK(store_write(&st, 1, big, 10) == STORE_READ_ONLY);
    CHECK(has(5, big, sizeof big));
}

static void test_wear_spreads(void)
{
    uint8_t v[1000];
    sim_reset(); power_on();
    for (unsigned i = 0; i < 1280; i++) {
        fill(v, sizeof v, i);
        CHECK(store_write(&st, (uint16_t)(i % 4), v, sizeof v) == STORE_OK);
    }
    unsigned lo = ~0u, hi = 0;
    for (unsigned s = 0; s < 128; s++) {
        if (sim.erases[s] < lo) lo = sim.erases[s];
        if (sim.erases[s] > hi) hi = sim.erases[s];
    }
    CHECK(hi - lo <= 1);
}

static void test_random_workload_with_cuts(void)
{
    enum { KEYS = 12 };
    static uint8_t model[KEYS][7000], pending[7000];
    static size_t model_len[KEYS];
    srand(23);
    sim_reset(); power_on();
    for (unsigned iter = 0; iter < 3000; iter++) {
        uint16_t key = (uint16_t)(rand() % KEYS);
        size_t len = (size_t)(rand() % 7000) + 1;
        fill(pending, len, iter);
        int cut = rand() % 4 == 0;
        if (cut) sim.budget = rand() % (long)(len + 40);
        int r = store_write(&st, key, pending, len);
        if (cut) {
            power_on();
            int is_new = has(key, pending, len);
            int is_old = model_len[key] ? has(key, model[key], model_len[key])
                                        : store_read(&st, key, pending, 0, NULL) == STORE_NOT_FOUND;
            CHECK(is_new || is_old);
            if (is_new) { memcpy(model[key], pending, len); model_len[key] = len; }
        } else {
            CHECK(r == STORE_OK);
            memcpy(model[key], pending, len); model_len[key] = len;
        }
        if (iter % 97 == 0) power_on();
        for (unsigned k = 0; k < KEYS; k++)
            if (model_len[k] && k != key) CHECK(has((uint16_t)k, model[k], model_len[k]));
        if (failures) { fprintf(stderr, "  at iteration %u\n", iter); return; }
    }
}

/* Stream an 80 KB "kick" in uneven chunks; replace it; cut power mid-stream. */
static void test_stream(void)
{
    enum { KICK = 0xF0, N = 80 * 1024 };
    static uint8_t a[N], b[N];
    fill(a, N, 40); fill(b, N, 41);
    sim_reset(); power_on();

    CHECK(store_begin(&st, KICK, N) == STORE_OK);
    CHECK(store_write(&st, 1, a, 10) == STORE_INVALID); /* busy */
    for (size_t at = 0; at < N;) {
        size_t n = (at * 7 + 333) % 700 + 1;
        if (n > N - at) n = N - at;
        CHECK(store_append(&st, a + at, n) == STORE_OK);
        at += n;
    }
    CHECK(store_append(&st, a, 1) == STORE_INVALID); /* past the length */
    CHECK(store_commit(&st, store_crc32(a, N, 0)) == STORE_OK);
    power_on();
    CHECK(has(KICK, a, N));
    uint32_t off, len;
    CHECK(store_locate(&st, KICK, &off, &len) == STORE_OK && len == N);
    CHECK(memcmp(sim.mem + off, a, N) == 0);

    /* Wrong CRC: refused, old kick stays. */
    CHECK(store_begin(&st, KICK, N) == STORE_OK);
    CHECK(store_append(&st, b, N) == STORE_OK);
    CHECK(store_commit(&st, 0x12345678u) == STORE_INVALID);
    power_on();
    CHECK(has(KICK, a, N));

    /* Short upload, then abandoned: old kick stays, other writes work again. */
    CHECK(store_begin(&st, KICK, N) == STORE_OK);
    CHECK(store_append(&st, b, N / 2) == STORE_OK);
    CHECK(store_commit(&st, store_crc32(b, N, 0)) == STORE_INVALID);
    CHECK(store_write(&st, 1, a, 10) == STORE_OK);
    CHECK(has(KICK, a, N));

    /* Power cut at many points of a replacement. */
    static uint8_t snapshot[FLASH_BYTES];
    power_on();
    memcpy(snapshot, sim.mem, FLASH_BYTES);
    unsigned saw_new = 0;
    for (long cut = 0; cut < N + 2000; cut += 997) {
        memcpy(sim.mem, snapshot, FLASH_BYTES);
        power_on();
        sim.budget = cut;
        int ok = store_begin(&st, KICK, N) == STORE_OK;
        for (size_t at = 0; ok && at < N; at += 512)
            ok = store_append(&st, b + at, N - at < 512 ? N - at : 512) == STORE_OK;
        if (ok) ok = store_commit(&st, store_crc32(b, N, 0)) == STORE_OK;
        power_on();
        CHECK(has(KICK, a, N) || has(KICK, b, N));
        CHECK(has(1, a, 10));
        saw_new += has(KICK, b, N);
    }
    CHECK(saw_new > 0);

    /* Read-only (unknown flash chip): refused. */
    CHECK(store_mount(&st, &flash, 1) == STORE_OK);
    CHECK(store_begin(&st, KICK, N) == STORE_READ_ONLY);
}

int main(void)
{
    CHECK(store_crc32("123456789", 9, 0) == 0xCBF43926u);
    test_roundtrip();
    test_overwrite_keeps_newest();
    test_power_cut_every_step();
    test_bit_flip_falls_back();
    test_full_and_read_only();
    test_wear_spreads();
    test_random_workload_with_cuts();
    test_stream();
    if (failures) { printf("test_store: %d failure(s)\n", failures); return 1; }
    printf("test_store: ok\n");
    return 0;
}
