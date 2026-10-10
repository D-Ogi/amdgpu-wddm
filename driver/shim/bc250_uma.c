// SPDX-License-Identifier: MIT
// PROVENANCE: fanoush/bc250_memcfg, MIT, 222420fb: block layout, signatures and checksum.
#include "bc250_uma.h"
#include <string.h>

static unsigned int word(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int checksum(const unsigned char* block)
{
    unsigned int sum = 0, i;
    for (i = 6; i < BC250_UMA_BLOCK_BYTES; ++i) sum += block[i];
    return sum;
}
int bc250_uma_validate(const unsigned char* block)
{
    unsigned int signature;
    if (!block) return 0;
    signature = word(block) | (word(block + 2) << 16);
    /* CMSB with valid checksum is the measured post-CMOS-clear board baseline. */
    if (signature != 0x4c424124u && signature != 0x42435041u && signature != 0x42534d43u)
        return 0;
    if (word(block + 26) < 256 || word(block + 26) >= 14336 ||
        (word(block + 26) & 15u) != 0) return 0;
    return word(block + 4) == checksum(block);
}
int bc250_uma_read(const struct bc250_uma_io* io, unsigned char* block)
{
    unsigned char first[BC250_UMA_BLOCK_BYTES], second[BC250_UMA_BLOCK_BYTES];
    unsigned int i;
    if (!io || !io->read || !block) return BC250_UMA_INVALID;
    for (i = 0; i < BC250_UMA_BLOCK_BYTES; ++i)
        if (!io->read(io->context, i, &first[i])) return BC250_UMA_READ_FAILED;
    for (i = 0; i < BC250_UMA_BLOCK_BYTES; ++i)
        if (!io->read(io->context, i, &second[i])) return BC250_UMA_READ_FAILED;
    if (memcmp(first, second, sizeof(first))) return BC250_UMA_UNSTABLE;
    memcpy(block, first, sizeof(first));
    return BC250_UMA_OK;
}
/* Invalid signature first; checksum and payload before signature commit byte0.
   Only signature, checksum and UMA bytes may ever be written, including rollback. */
static int publish(const struct bc250_uma_io* io, const unsigned char* block,
                   unsigned int* attempts, int best_effort)
{
    static const unsigned int offsets[] = {26, 27, 4, 5, 1, 2, 3, 0};
    unsigned int i;
    int ok;
    ++*attempts;
    ok = io->write(io->context, 0, 0);
    /* Without confirmed invalidation, never modify a potentially live block. */
    if (!ok) return 0;
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        int written;
        /* A rollback may attempt the remaining data bytes, but cannot publish
           a valid signature after any unconfirmed earlier write. */
        if (offsets[i] == 0 && !ok) break;
        ++*attempts;
        written = io->write(io->context, offsets[i], block[offsets[i]]);
        ok = ok && written;
        if (!written && !best_effort) return 0;
    }
    return ok;
}
int bc250_uma_apply(const struct bc250_uma_io* io, const unsigned char* expected,
                    unsigned int target_mib, struct bc250_uma_result* result)
{
    unsigned char current[BC250_UMA_BLOCK_BYTES], candidate[BC250_UMA_BLOCK_BYTES];
    unsigned int sum;
    int status, restored;
    if (result) memset(result, 0, sizeof(*result));
    if (!io || !io->read || !io->write || !result ||
        (target_mib != 8192 && target_mib != 12288) || !bc250_uma_validate(expected))
        return BC250_UMA_INVALID;
    status = bc250_uma_read(io, current);
    if (status != BC250_UMA_OK) return status;
    if (memcmp(current, expected, sizeof(current))) return BC250_UMA_STALE;
    if (word(current + 26) == target_mib) return BC250_UMA_NO_CHANGE;
    memcpy(candidate, current, sizeof(candidate));
    candidate[26] = (unsigned char)target_mib;
    candidate[27] = (unsigned char)(target_mib >> 8);
    sum = checksum(candidate);
    candidate[4] = (unsigned char)sum;
    candidate[5] = (unsigned char)(sum >> 8);
    candidate[0] = 0x41; candidate[1] = 0x50; candidate[2] = 0x43; candidate[3] = 0x42;
    if (publish(io, candidate, &result->writes_attempted, 0) &&
        bc250_uma_read(io, current) == BC250_UMA_OK &&
        !memcmp(current, candidate, sizeof(current))) return BC250_UMA_OK;
    result->operation_failed = 1;
    restored = publish(io, expected, &result->rollback_writes_attempted, 1);
    if (bc250_uma_read(io, current) != BC250_UMA_OK ||
        memcmp(current, expected, sizeof(current))) restored = 0;
    return restored ? BC250_UMA_RESTORED : BC250_UMA_ROLLBACK_UNCONFIRMED;
}
