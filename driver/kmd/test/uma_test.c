// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "bc250_uma.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
struct mock {
    unsigned char bytes[28];
    unsigned reads, writes, fail_write, fail_read;
    int write_then_fail, persistent, mutate_read, corrupt_verify, ignore_invalidation;
};
static int read_byte(void* ctx, unsigned offset, unsigned char* value)
{
    struct mock* m = ctx;
    ++m->reads;
    CHECK(offset < 28);
    if (m->fail_read == m->reads) return 0;
    if (m->mutate_read && m->reads == 29) m->bytes[8] ^= 1;
    if (m->corrupt_verify && m->reads == 57) m->bytes[8] ^= 1;
    *value = m->bytes[offset];
    return 1;
}
static int write_byte(void* ctx, unsigned offset, unsigned char value)
{
    struct mock* m = ctx;
    int failed;
    ++m->writes;
    CHECK(offset < 6 || offset == 26 || offset == 27);
    CHECK(offset == 0 || m->bytes[offset] != value);
    failed = m->fail_write && (m->writes == m->fail_write ||
                             (m->persistent && m->writes >= m->fail_write));
    if ((!failed || m->write_then_fail) && !(m->ignore_invalidation && offset == 0 && value == 0))
        m->bytes[offset] = value;
    return !failed;
}
static void init(struct mock* m, unsigned signature, unsigned mib)
{
    unsigned i, sum = 0;
    memset(m, 0, sizeof(*m));
    for (i = 0; i < 4; ++i) m->bytes[i] = (unsigned char)(signature >> (i * 8));
    for (i = 6; i < 26; ++i) m->bytes[i] = (unsigned char)(i * 7);
    m->bytes[26] = (unsigned char)mib; m->bytes[27] = (unsigned char)(mib >> 8);
    for (i = 6; i < 28; ++i) sum += m->bytes[i];
    m->bytes[4] = (unsigned char)sum; m->bytes[5] = (unsigned char)(sum >> 8);
}
int main(void)
{
    struct mock m;
    struct bc250_uma_io io = {&m, read_byte, write_byte};
    struct bc250_uma_result result;
    unsigned char expected[28], readback[28];
    unsigned i, j, signature;
    const unsigned signatures[] = {0x4c424124, 0x42435041, 0x42534d43};
    const unsigned invalid[] = {0, 0xffffffff, 0x46544457, 0x454b4843, 0x45474953};
    for (signature = 0; signature < 3; ++signature) {
        init(&m, signatures[signature], 8192); memcpy(expected, m.bytes, 28);
        CHECK(bc250_uma_validate(expected));
        CHECK(bc250_uma_apply(&io, expected, 8192, &result) == BC250_UMA_NO_CHANGE);
        CHECK(m.writes == 0 && !result.operation_failed);
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_OK);
        CHECK(m.writes == (signature == 0 ? 7u : signature == 1 ? 4u : 6u));
        CHECK(result.writes_attempted == m.writes && !result.rollback_writes_attempted);
        CHECK(m.bytes[26] == 0 && m.bytes[27] == 0x30 && bc250_uma_validate(m.bytes));
        CHECK(!memcmp(m.bytes + 6, expected + 6, 20));
        CHECK(m.bytes[0] == 'A' && m.bytes[1] == 'P' && m.bytes[2] == 'C' && m.bytes[3] == 'B');
        memcpy(expected, m.bytes, 28);
        CHECK(bc250_uma_apply(&io, expected, 8192, &result) == BC250_UMA_OK);
        CHECK(m.bytes[27] == 0x20 && bc250_uma_validate(m.bytes));
    }
    {
        const unsigned bad_sizes[] = {0, 255, 257, 8193, 14335, 14336, 16384, 65535};
        const unsigned valid_sizes[] = {256, 272, 4096, 8192, 12288, 14320};
        for (i = 0; i < sizeof(bad_sizes)/sizeof(bad_sizes[0]); ++i) {
            init(&m, signatures[0], bad_sizes[i]); memcpy(expected, m.bytes, 28);
            CHECK(!bc250_uma_validate(expected));
            CHECK(bc250_uma_apply(&io, expected, 8192, &result) == BC250_UMA_INVALID);
            CHECK(m.writes == 0 && m.reads == 0);
        }
        for (i = 0; i < sizeof(valid_sizes)/sizeof(valid_sizes[0]); ++i) {
            init(&m, signatures[0], valid_sizes[i]); memcpy(expected, m.bytes, 28);
            CHECK(bc250_uma_validate(expected));
            CHECK(bc250_uma_apply(&io, expected, 12288, &result) ==
                  (valid_sizes[i] == 12288 ? BC250_UMA_NO_CHANGE : BC250_UMA_OK));
        }
    }
    for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        init(&m, invalid[i], 8192); memcpy(expected, m.bytes, 28);
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_INVALID && !m.writes);
    }
    init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); expected[8] ^= 1;
    CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_INVALID && !m.writes);
    init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); m.bytes[8] ^= 1;
    CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_STALE && !m.writes);
    for (i = 0; i < 16385; ++i) if (i != 8192 && i != 12288) {
        init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28);
        CHECK(bc250_uma_apply(&io, expected, i, &result) == BC250_UMA_INVALID && !m.writes && !m.reads);
    }
    for (i = 1; i <= 56; ++i) {
        init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); m.fail_read = i;
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_READ_FAILED && !m.writes);
    }
    init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); m.mutate_read = 1;
    CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_UNSTABLE && !m.writes);
    for (i = 1; i <= 7; ++i) for (j = 0; j != 2; ++j) {
        init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28);
        m.fail_write = i; m.write_then_fail = (int)j;
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_RESTORED);
        CHECK(!memcmp(m.bytes, expected, 28) && result.operation_failed);
        CHECK(result.writes_attempted == i && result.rollback_writes_attempted >= 2);
        init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28);
        m.fail_write = i; m.persistent = 1;
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
    }
    for (i = 57; i <= 120; ++i) {
        init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); m.fail_read = i;
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_RESTORED);
        CHECK(!memcmp(m.bytes, expected, 28));
    }
    /* First verification read follows the marker witness and seven comparison reads. The successful
       apply and exact rollback each perform seven writes for this signature. */
    for (i = 8; i <= 14; ++i) {
        init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28);
        m.fail_read = 65; m.fail_write = i;
        CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
        CHECK(result.writes_attempted == 7);
        if (i > 8 && i < 14) CHECK(m.bytes[0] == 0);
    }
    /* A rollback read failure cannot report restoration, even if bytes are equal. */
    init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28);
    m.fail_write = 1; m.fail_read = 57;
    CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
    init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); m.corrupt_verify = 1;
    CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
    CHECK(m.bytes[8] != expected[8]); /* Never repair a non-UMA byte as collateral damage. */
    CHECK(bc250_uma_read(NULL, readback) == BC250_UMA_INVALID);
    CHECK(bc250_uma_apply(&io, NULL, 8192, &result) == BC250_UMA_INVALID);
    CHECK(bc250_uma_apply(&io, expected, 8192, NULL) == BC250_UMA_INVALID);
    /* A HAL success count without an effective marker write is not admission. */
    init(&m, signatures[0], 8192); memcpy(expected, m.bytes, 28); m.ignore_invalidation = 1;
    CHECK(bc250_uma_apply(&io, expected, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
    CHECK(m.writes == 2 && !memcmp(m.bytes, expected, 28));
    /* Exact backup restoration preserves all three accepted signatures. */
    for (signature = 0; signature < 3; ++signature) {
        init(&m, signatures[signature], 8192); memcpy(readback, m.bytes, 28);
        init(&m, signatures[1], 12288); memcpy(expected, m.bytes, 28);
        CHECK(bc250_uma_restore(&io, expected, readback, &result) == BC250_UMA_OK);
        CHECK(!memcmp(m.bytes, readback, 28));
        m.writes = 0;
        CHECK(bc250_uma_restore(&io, readback, readback, &result) == BC250_UMA_NO_CHANGE);
        CHECK(m.writes == 0);
    }
    for (i = 1; i <= 6; ++i) for (j = 0; j < 2; ++j) {
        init(&m, signatures[2], 8192); memcpy(readback, m.bytes, 28);
        init(&m, signatures[1], 12288); memcpy(expected, m.bytes, 28);
        m.fail_write = i; m.write_then_fail = (int)j;
        CHECK(bc250_uma_restore(&io, expected, readback, &result) == BC250_UMA_RESTORED);
        CHECK(!memcmp(m.bytes, expected, 28));
        init(&m, signatures[1], 12288); m.fail_write = i; m.persistent = 1;
        CHECK(bc250_uma_restore(&io, expected, readback, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
    }
    init(&m, signatures[2], 4096); memcpy(readback, m.bytes, 28);
    init(&m, signatures[1], 12288); memcpy(expected, m.bytes, 28);
    CHECK(bc250_uma_restore(&io, expected, readback, &result) == BC250_UMA_INVALID);
    CHECK(m.reads == 0 && m.writes == 0);
    init(&m, signatures[2], 8192); memcpy(readback, m.bytes, 28);
    init(&m, signatures[1], 12288); memcpy(expected, m.bytes, 28);
    /* Valid checksum but different timing profile must still be rejected. */
    ++readback[8]; ++readback[4];
    CHECK(bc250_uma_validate(readback));
    CHECK(bc250_uma_restore(&io, expected, readback, &result) == BC250_UMA_INVALID);
    CHECK(m.reads == 0 && m.writes == 0);
    --readback[8]; --readback[4]; m.bytes[8] ^= 1;
    CHECK(bc250_uma_restore(&io, expected, readback, &result) == BC250_UMA_STALE);
    CHECK(m.writes == 0);
    printf("UMA policy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
