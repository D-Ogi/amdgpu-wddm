#ifndef VKMEMBW_CHECK_H
#define VKMEMBW_CHECK_H
#include <stddef.h>
#include <stdint.h>

static uint32_t Pattern(uint64_t e, uint32_t lane, uint32_t seed)
{
    return ((uint32_t)(4u * (uint32_t)e) + lane) ^ seed;
}

/* ByteOffset and Bytes are word aligned. Check every word, including interior pages. */
static uint32_t CheckWords(const uint32_t *words, uint64_t byteOffset, size_t bytes, uint32_t seed)
{
    uint32_t mismatches = 0;
    for (size_t w = 0; w < bytes / 4; w++) {
        uint64_t at = byteOffset / 4 + w;
        if (words[w] != Pattern(at / 4, (uint32_t)(at & 3), seed)) mismatches++;
    }
    return mismatches;
}
#endif
