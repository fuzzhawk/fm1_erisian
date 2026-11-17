/* deps: app/storage/upload.c app/storage/store.c app/midi/sysex.c */
#include "../app/storage/upload.h"
#include "../app/midi/sysex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* 440 KB like ERISIAN's share of the user partition. */
#define FLASH_BYTES (110u * STORE_SECTOR_BYTES)
static uint8_t mem[FLASH_BYTES];
static int sim_read(void *c, uint32_t o, void *d, size_t n) { (void)c; memcpy(d, mem + o, n); return 0; }
static int sim_write(void *c, uint32_t o, const void *d, size_t n)
{
    (void)c; const uint8_t *p = d;
    for (size_t i = 0; i < n; i++) { if ((mem[o + i] & p[i]) != p[i]) return -4; mem[o + i] &= p[i]; }
    return 0;
}
static int sim_erase(void *c, uint32_t o) { (void)c; memset(mem + o, 0xFF, STORE_SECTOR_BYTES); return 0; }
static struct store_flash flash = { NULL, FLASH_BYTES, sim_read, sim_write, sim_erase };
static struct store st;
static struct upload up;

struct ack { int ours; uint8_t cmd, status; uint32_t value; };

static struct ack send(const uint8_t *f, size_t n)
{
    uint8_t reply[UPLOAD_REPLY_MAX], raw[8];
    size_t rn = 0;
    struct ack a = {0};
    a.ours = upload_handle(&up, f, n, reply, &rn);
    if (!a.ours) return a;
    CHECK(rn <= UPLOAD_REPLY_MAX && reply[0] == 0xF0 && reply[rn - 1] == 0xF7 && reply[4] == UPLOAD_ACK);
    for (size_t i = 1; i + 1 < rn; i++) CHECK(reply[i] < 0x80);
    size_t m = sysex_unpack(reply + 5, rn - 6, raw, sizeof raw);
    CHECK(m == 6);
    a.cmd = raw[0]; a.status = raw[1];
    a.value = (uint32_t)raw[2] | (uint32_t)raw[3] << 8 | (uint32_t)raw[4] << 16 | (uint32_t)raw[5] << 24;
    return a;
}

static size_t build(uint8_t *f, uint8_t cmd, const uint8_t *body, size_t n)
{
    size_t o = 0;
    f[o++] = 0xF0; f[o++] = 0x7D; f[o++] = 0x45; f[o++] = 0x52; f[o++] = cmd;
    o += sysex_pack(body, n, f + o);
    f[o++] = 0xF7;
    return o;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static struct ack cmd0(uint8_t cmd) { uint8_t f[8]; return send(f, build(f, cmd, NULL, 0)); }

static struct ack begin(uint8_t slot, uint32_t len, uint32_t crc)
{
    uint8_t b[9], f[32];
    b[0] = slot; put32(b + 1, len); put32(b + 5, crc);
    return send(f, build(f, UPLOAD_BEGIN, b, 9));
}

static struct ack data(const uint8_t *payload, uint32_t offset, size_t n)
{
    static uint8_t b[4 + UPLOAD_CHUNK_BYTES], f[1024];
    put32(b, offset); memcpy(b + 4, payload + offset, n);
    return send(f, build(f, UPLOAD_DATA, b, 4 + n));
}

static size_t make_payload(uint8_t *p, uint32_t frames, uint32_t rate, unsigned seed)
{
    struct sample_header h = { SAMPLE_MAGIC, 1, 1, rate, frames };
    memcpy(p, &h, sizeof h);
    for (uint32_t i = 0; i < frames * 2; i++) p[sizeof h + i] = (uint8_t)(seed * 31 + i * 13 + (i >> 9));
    return sizeof h + frames * 2;
}

static int stored_is(const uint8_t *p, size_t n)
{
    static uint8_t buf[UPLOAD_MAX_BYTES];
    size_t len;
    return store_read(&st, UPLOAD_KEY(0), buf, sizeof buf, &len) == STORE_OK && len == n && !memcmp(buf, p, n);
}

static int upload_all(const uint8_t *p, size_t n)
{
    if (begin(0, (uint32_t)n, store_crc32(p, n, 0)).status) return 0;
    for (size_t o = 0; o < n; o += UPLOAD_CHUNK_BYTES) {
        size_t c = n - o < UPLOAD_CHUNK_BYTES ? n - o : UPLOAD_CHUNK_BYTES;
        struct ack a = data(p, (uint32_t)o, c);
        if (a.status || a.value != o + c) return 0;
    }
    struct ack e = cmd0(UPLOAD_END);
    return e.status == 0 && e.value == n;
}

static void test_pack(void)
{
    uint8_t in[600], packed[SYSEX_PACKED_SIZE(600)], out[600];
    for (unsigned i = 0; i < sizeof in; i++) in[i] = (uint8_t)(i * 97 + 200);
    for (size_t n = 0; n <= 600; n += (n < 20 ? 1 : 97)) {
        size_t p = sysex_pack(in, n, packed);
        CHECK(p == SYSEX_PACKED_SIZE(n));
        for (size_t i = 0; i < p; i++) CHECK(packed[i] < 0x80);
        CHECK(sysex_unpack(packed, p, out, sizeof out) == n && !memcmp(in, out, n));
    }
    packed[0] = 0x80;
    CHECK(sysex_unpack(packed, 8, out, sizeof out) == (size_t)-1);
    CHECK(sysex_unpack(packed + 1, 1, out, sizeof out) == (size_t)-1); /* lone header byte */
}

static void test_protocol(void)
{
    static uint8_t a[UPLOAD_MAX_BYTES], b[UPLOAD_MAX_BYTES];
    memset(mem, 0xFF, sizeof mem);
    CHECK(store_mount(&st, &flash, 0) == STORE_OK);
    upload_init(&up, &st);

    uint8_t other[] = { 0xF0, 0x43, 0x10, 0x01, 0xF7 };
    CHECK(send(other, sizeof other).ours == 0);
    struct ack h = cmd0(UPLOAD_HELLO);
    CHECK(h.ours && h.status == 0 && h.value == UPLOAD_PROTOCOL);

    size_t na = make_payload(a, 40000, 44100, 1);
    CHECK(cmd0(UPLOAD_END).status == UPLOAD_E_STATE);
    CHECK(data(a, 0, 100).status == UPLOAD_E_STATE);
    CHECK(upload_all(a, na));
    CHECK(stored_is(a, na));

    /* Errors leave the stored kick alone. */
    size_t nb = make_payload(b, 40000, 44100, 2);
    CHECK(begin(1, (uint32_t)nb, 0).status == UPLOAD_E_ARGUMENT);
    CHECK(begin(0, UPLOAD_MAX_BYTES + 1, 0).status == UPLOAD_E_ARGUMENT);
    CHECK(begin(0, (uint32_t)nb, store_crc32(b, nb, 0)).status == 0);
    CHECK(data(b, 0, 512).value == 512);
    CHECK(data(b, 0, 512).status == 0);                 /* duplicate: fine */
    struct ack gap = data(b, 2048, 512);
    CHECK(gap.status == UPLOAD_E_OFFSET && gap.value == 512);
    CHECK(cmd0(UPLOAD_END).status == UPLOAD_E_CRC);     /* incomplete */
    CHECK(stored_is(a, na));

    CHECK(begin(0, (uint32_t)nb, 0xDEADBEEFu).status == 0);  /* wrong CRC */
    for (size_t o = 0; o < nb; o += 512) data(b, (uint32_t)o, nb - o < 512 ? nb - o : 512);
    CHECK(cmd0(UPLOAD_END).status == UPLOAD_E_CRC);
    CHECK(stored_is(a, na));

    size_t nw = make_payload(b, 40000, 48000, 3);       /* wrong rate in header */
    CHECK(begin(0, (uint32_t)nw, store_crc32(b, nw, 0)).status == 0);
    CHECK(data(b, 0, 512).status == UPLOAD_E_ARGUMENT);
    CHECK(data(b, 512, 512).status == UPLOAD_E_STATE);  /* upload was dropped */
    CHECK(stored_is(a, na));

    CHECK(begin(0, (uint32_t)nb, 0).status == 0);
    CHECK(cmd0(UPLOAD_ABORT).status == 0);
    CHECK(data(b, 0, 512).status == UPLOAD_E_STATE);
    CHECK(stored_is(a, na));

    uint8_t truncated[] = { 0xF0, 0x7D, 0x45, 0x52, UPLOAD_HELLO, 0x00 };
    CHECK(send(truncated, sizeof truncated).status == UPLOAD_E_FRAME);

    /* Replace with b, then remount: b is what survives. */
    nb = make_payload(b, 45000, 44100, 4);
    CHECK(upload_all(b, nb));
    CHECK(store_mount(&st, &flash, 0) == STORE_OK);
    CHECK(stored_is(b, nb));

    /* Unknown flash chip: mounted read-only, uploads refused. */
    CHECK(store_mount(&st, &flash, 1) == STORE_OK);
    upload_init(&up, &st);
    CHECK(begin(0, (uint32_t)na, store_crc32(a, na, 0)).status == UPLOAD_E_FLASH);
    CHECK(stored_is(b, nb));
}

/* Replay the stream tools/uploader/erisian-upload.js produces. */
static void test_js_frames(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("  (no %s: JS frame test skipped)\n", path); return; }
    static uint8_t file[512 * 1024];
    size_t n = fread(file, 1, sizeof file, fp);
    fclose(fp);
    memset(mem, 0xFF, sizeof mem);
    CHECK(store_mount(&st, &flash, 0) == STORE_OK);
    upload_init(&up, &st);
    uint32_t count = (uint32_t)file[0] | (uint32_t)file[1] << 8;
    size_t o = 4;
    for (uint32_t i = 0; i < count; i++) {
        size_t len = (size_t)file[o] | (size_t)file[o + 1] << 8;
        o += 2;
        struct ack a = send(file + o, len);
        CHECK(a.ours && a.status == 0);
        if (a.status) { fprintf(stderr, "  JS frame %u: status %u\n", i, a.status); return; }
        o += len;
    }
    CHECK(stored_is(file + o, n - o));
    printf("  JS uploader frames accepted (%u frames, %zu-byte sample)\n", count, n - o);
}

int main(int argc, char **argv)
{
    test_pack();
    test_protocol();
    test_js_frames(argc > 1 ? argv[1] : "build-host/js_frames.bin");
    if (failures) { printf("test_upload: %d failure(s)\n", failures); return 1; }
    printf("test_upload: ok\n");
    return 0;
}
