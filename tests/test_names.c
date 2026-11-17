/* deps: app/ui/names.c */
#include "../app/ui/names.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    uint32_t seed = 5;
    char name[NAME_MAX_CHARS + 1], first[NAME_MAX_CHARS + 1];
    unsigned distinct = 0;
    char seen[200][NAME_MAX_CHARS + 1];
    for (int i = 0; i < 200; i++) {
        name_generate(&seed, name);
        size_t len = strlen(name);
        CHECK(len >= 1 && len <= NAME_MAX_CHARS);
        for (size_t k = 0; k < len; k++)
            CHECK(strchr(name_charset, name[k]) != NULL && name[k] != 0);
        int dup = 0;
        for (unsigned j = 0; j < distinct; j++)
            dup |= !strcmp(seen[j], name);
        if (!dup)
            strcpy(seen[distinct++], name);
        if (i < 8)
            printf("  %s\n", name);
    }
    CHECK(distinct > 150);

    /* Same seed, same name: deterministic. */
    uint32_t s1 = 23, s2 = 23;
    name_generate(&s1, first);
    name_generate(&s2, name);
    CHECK(!strcmp(first, name));

    /* Character editing. */
    strcpy(name, "ERIS");
    CHECK(name_edit(name, 0, 1) == 4 && !strcmp(name, "FRIS"));
    CHECK(name_edit(name, 0, -1) == 4 && !strcmp(name, "ERIS"));
    CHECK(name_edit(name, 6, 1) == 7 && !strcmp(name, "ERIS  A"));
    CHECK(name_edit(name, 6, -1) == 4 && !strcmp(name, "ERIS")); /* back to space: trimmed */
    strcpy(name, "A");
    name_edit(name, 0, -2); /* wraps: A -> ' ' -> '-' */
    CHECK(!strcmp(name, "-"));
    CHECK(name_edit(name, NAME_MAX_CHARS, 1) == 1);

    if (failures) { printf("test_names: %d failure(s)\n", failures); return 1; }
    printf("test_names: ok\n");
    return 0;
}
