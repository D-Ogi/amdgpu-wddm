/* Synthetic SMBIOS only; no firmware or OS queries. */
#include <stdio.h>
#include <string.h>
#include "../../driver/kmd/board_identity.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u\n", (unsigned)__LINE__); } } while (0)
static unsigned char table[512];
static size_t fixture(void) {
    static const char strings[] = "American Megatrends Inc.\0P3.00\0" "12/09/2021\0";
    size_t at = 8; unsigned i;
    memset(table, 0, sizeof(table)); table[1] = 2; table[2] = 6;
    table[at + 1] = 9; table[at + 4] = 1; table[at + 5] = 2; table[at + 8] = 3;
    memcpy(table + at + 9, strings, sizeof(strings)); at += 9 + sizeof(strings);
    table[at] = 1; table[at + 1] = 24;
    for (i = 0; i < 16; ++i) table[at + 8 + i] = (unsigned char)(i + 1);
    at += 26; table[at] = 127; table[at + 1] = 4; at += 6;
    table[4] = (unsigned char)(at - 8); return at;
}
static void refused(size_t bytes) {
    unsigned bios = 99; unsigned char id[16]; unsigned i;
    memset(id, 99, sizeof(id));
    CHECK(!BoardIdentityQualification(table, bytes, &bios, id)); CHECK(bios == 0);
    for (i = 0; i < 16; ++i) CHECK(id[i] == 0);
}
int main(void) {
    size_t n = fixture(), type1 = n - 32; unsigned bios = 0, i; unsigned char id[16];
    CHECK(BoardIdentityQualification(table, n, &bios, id)); CHECK(bios == 3);
    for (i = 0; i < 16; ++i) CHECK(id[i] == i + 1);
    for (i = 0; i < n; ++i) refused(i);
    n = fixture(); table[2] = 5; refused(n);
    n = fixture(); memset(table + type1 + 8, 0, 16); refused(n);
    n = fixture(); memset(table + type1 + 8, 255, 16); refused(n);
    n = fixture(); table[type1 + 1] = 23; refused(n);
    n = fixture(); table[type1] = 3; refused(n);
    n = fixture(); table[8] = 3; refused(n);
    n = fixture(); table[17] = 'X'; refused(n);
    n = fixture(); table[n - 6] = 3; refused(n);
    n = fixture(); memmove(table + type1 + 26, table + type1, n - type1); n += 26; table[4] = (unsigned char)(n - 8); refused(n);
    n = fixture(); memmove(table + type1, table + 8, n - 8); n += type1 - 8; table[4] = (unsigned char)(n - 8); refused(n);
    printf("%u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
}
