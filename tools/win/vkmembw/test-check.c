/* Host-only content oracle controls. Does not execute Vulkan or establish GPU correctness. */
#include "check.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const size_t bytes = 4u << 20, words = bytes / 4;
    const uint32_t seed = 0x9e3779b8u;
    uint32_t *data = (uint32_t *)calloc(words, sizeof(*data));
    unsigned checks = 0, failures = 0;
    const uint64_t offsets[] = { 0, 4ull << 20, (4ull << 30) - bytes };
    if (!data) return 2;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %u\n", (unsigned)__LINE__); } } while (0)
    CHECK(Pattern(0, 0, seed) == 0x9e3779b8u);
    CHECK(Pattern(0, 1, seed) == 0x9e3779b9u);
    for (size_t b = 0; b < sizeof(offsets) / sizeof(offsets[0]); b++) {
        for (size_t w = 0; w < words; w++) data[w] = (uint32_t)(offsets[b] / 4 + w) ^ seed;
        CHECK(CheckWords(data, offsets[b], bytes, seed) == 0);
        CHECK(CheckWords(data, offsets[b], bytes, seed ^ 1u) == words); /* stale warmup */
        const size_t positions[] = { 0, words / 2, words - 1 };
        for (size_t p = 0; p < sizeof(positions) / sizeof(positions[0]); p++) {
            data[positions[p]] ^= 1u;
            CHECK(CheckWords(data, offsets[b], bytes, seed) == 1);
            data[positions[p]] ^= 1u;
        }
        /* The same full region split into chunks, preserving the absolute pattern offset. */
        CHECK(CheckWords(data, offsets[b], bytes / 2, seed) == 0);
        CHECK(CheckWords(data + words / 2, offsets[b] + bytes / 2, bytes / 2, seed) == 0);
    }
    for (size_t w = 0; w < words; w++) data[w] = 0;
    CHECK(CheckWords(data, 0, bytes, seed) == words); /* poisoned copy with no dispatch */
    free(data);
    printf("vkmembw content controls: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
