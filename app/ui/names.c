/* SPDX-License-Identifier: MIT */
/* An order-2 character Markov chain over an original Discordian word list.
 * Transitions are counted from the list on demand: no RAM table, only the
 * word list in flash. */
#include "names.h"

#include <string.h>

const char name_charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";

static const char *const corpus[] = {
    "ERIS", "KALLISTI", "FNORD", "DISCORDIA", "PENTABARF", "GREYFACE",
    "CHAO", "HODGE", "PODGE", "MALACLYPSE", "OMAR", "HAIL", "APPLE",
    "GOLDEN", "HOTDOG", "CHAOS", "DISCORD", "EPISKOPOS", "POPE", "CABBAGE",
    "BUREAUCRACY", "CONFUSION", "AFTERMATH", "SWEETMORN", "BOOMTIME",
    "SETTING", "ORANGE", "PINEAL", "ILLUMINATI", "SACRED", "ANERISTIC",
    "ERISTIC", "GODDESS", "STRIFE", "PRANK", "PENTAGON", "LEMMING",
    "NONSENSE", "ZENARCHY", "MUNGDAY", "SYADDAY", "MOJODAY", "CHAOFLUX",
};

static const char symbols[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.";
#define NSYM 27

static int index_of(char c)
{
    const char *p = strchr(symbols, c);
    return p ? (int)(p - symbols) : NSYM - 1;
}

/* Count which symbols follow the pair (a, b) in the corpus (END marks the
 * word boundary). Scanning the ~300-character list per letter is cheap and
 * needs no RAM table. */
static void follow(int a, int b, uint8_t row[NSYM])
{
    memset(row, 0, NSYM);
    for (unsigned w = 0; w < sizeof corpus / sizeof corpus[0]; w++) {
        int x = NSYM - 1, y = NSYM - 1;
        for (const char *p = corpus[w];; p++) {
            int c = *p ? index_of(*p) : NSYM - 1;
            if (x == a && y == b && row[c] < 255)
                row[c]++;
            if (!*p)
                break;
            x = y;
            y = c;
        }
    }
}

static uint32_t next(uint32_t *seed)
{
    /* xorshift32; a zero seed is replaced so it never sticks. */
    uint32_t x = *seed ? *seed : 0x23F00DD5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

static int pick(const uint8_t row[NSYM], uint32_t r, int allow_end)
{
    unsigned total = 0;
    for (int c = 0; c < NSYM; c++)
        total += (c == NSYM - 1 && !allow_end) ? 0 : row[c];
    if (!total)
        return -1;
    unsigned target = r % total;
    for (int c = 0; c < NSYM; c++) {
        unsigned n = (c == NSYM - 1 && !allow_end) ? 0 : row[c];
        if (target < n)
            return c;
        target -= n;
    }
    return -1;
}

static void sanitise(char *out)
{
    /* Keep only name_charset characters (generator output already is). */
    for (char *p = out; *p; p++)
        if (!strchr(name_charset, *p))
            *p = '-';
}

void name_generate(uint32_t *seed, char out[NAME_MAX_CHARS + 1])
{
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned len = 0;
        int a = NSYM - 1, b = NSYM - 1;
        unsigned limit = 5 + next(seed) % 6; /* aim for 5..10 letters */
        while (len < NAME_MAX_CHARS) {
            uint8_t row[NSYM];
            follow(a, b, row);
            int c = pick(row, next(seed), len >= 4);
            if (c < 0 || c == NSYM - 1)
                break;
            out[len++] = symbols[c];
            a = b;
            b = c;
            if (len >= limit) {
                /* Prefer to end at a natural word end, else stop. */
                uint8_t row[NSYM];
                follow(a, b, row);
                if (row[NSYM - 1] || len >= limit + 2)
                    break;
            }
        }
        out[len] = 0;
        /* Reject names that are exactly a corpus word: we want new ones. */
        int copy = 0;
        for (unsigned w = 0; w < sizeof corpus / sizeof corpus[0]; w++)
            copy |= strcmp(out, corpus[w]) == 0;
        if (len >= 3 && !copy)
            break;
    }
    /* Sometimes bless the name with 5 or 23. */
    uint32_t r = next(seed) % 9;
    unsigned len = (unsigned)strlen(out);
    const char *suffix = r == 0 ? "-23" : r == 1 ? "-5" : r == 2 ? "23" : "";
    size_t extra = strlen(suffix);
    if (extra && len + extra <= NAME_MAX_CHARS)
        strcpy(out + len, suffix);
    sanitise(out);
}

unsigned name_edit(char name[NAME_MAX_CHARS + 1], unsigned position, int delta)
{
    unsigned len = (unsigned)strlen(name);
    if (position >= NAME_MAX_CHARS)
        return len;
    while (len <= position)
        name[len++] = ' ';
    name[len] = 0;
    const int n = (int)(sizeof name_charset - 1);
    const char *p = strchr(name_charset, name[position]);
    int i = p ? (int)(p - name_charset) : 0;
    i = ((i + delta) % n + n) % n;
    name[position] = name_charset[i];
    while (len && name[len - 1] == ' ')
        name[--len] = 0;
    return len;
}
