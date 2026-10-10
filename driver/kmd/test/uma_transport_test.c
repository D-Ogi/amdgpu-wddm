#include <stdio.h>
#include <string.h>
#include <ntddk.h>
#include "uma_transport.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
static unsigned char memory[28];
static unsigned calls, reads, writes, mapping, fail_read, fail_write, bad_count;
static unsigned fail_all_writes, corrupt_read, irql, wrong_byte_view;
unsigned char KeGetCurrentIrql(void) { return (unsigned char)irql; }
static ULONG hal(int write, unsigned kind, unsigned bus, unsigned slot,
                 void* buffer, unsigned offset, unsigned bytes)
{
    unsigned address = mapping == 1 ? slot : offset;
    ++calls;
    CHECK(kind == Cmos && bus == 1);
    CHECK(mapping == 1 ? offset == 0 : slot == 0);
    CHECK(address >= 0x90 && address <= 0xab && bytes <= 0xac - address);
    if (address < 0x90 || address > 0xab || bytes > 0xac - address) return 0;
    if (write) {
        ++writes;
        CHECK(bytes == 1 && (address < 0x96 || address >= 0xaa));
        if (writes == fail_write || fail_all_writes) return bad_count;
        memcpy(memory + address - 0x90, buffer, bytes);
    } else {
        ++reads;
        if (reads == fail_read) return bad_count;
        memcpy(buffer, memory + address - 0x90, bytes);
        if (reads == corrupt_read || (wrong_byte_view && bytes == 1)) ((unsigned char*)buffer)[0] ^= 1;
    }
    return bytes;
}
ULONG HalGetBusDataByOffset(unsigned kind, ULONG bus, ULONG slot, void* b, ULONG o, ULONG n)
{ return hal(0, kind, bus, slot, b, o, n); }
ULONG HalSetBusDataByOffset(unsigned kind, ULONG bus, ULONG slot, void* b, ULONG o, ULONG n)
{ return hal(1, kind, bus, slot, b, o, n); }
static void init(unsigned map)
{
    unsigned i, sum = 0;
    mapping = map; calls = reads = writes = fail_read = fail_write = bad_count = 0;
    fail_all_writes = corrupt_read = irql = wrong_byte_view = 0;
    memcpy(memory, "CMSB", 4);
    for (i = 6; i < 26; ++i) memory[i] = (unsigned char)(i * 7);
    memory[26] = 0; memory[27] = 0x20;
    for (i = 6; i < 28; ++i) sum += memory[i];
    memory[4] = (unsigned char)sum; memory[5] = (unsigned char)(sum >> 8);
}
int main(void)
{
    struct bc250_uma_transport transport;
    struct bc250_uma_io io;
    struct bc250_uma_result result;
    unsigned char block[28], original[28], expected[28], value;
    unsigned map, i, count;
    for (map = 1; map <= 2; ++map) {
        transport.mapping = map; bc250_uma_transport_io(&transport, &io);
        init(map); memcpy(original, memory, 28);
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_OK);
        CHECK(reads == 58 && writes == 0 && !memcmp(block, original, 28));
        CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_OK);
        CHECK(memory[27] == 0x30 && !memcmp(memory + 6, original + 6, 20));
        memcpy(expected, memory, 28);
        CHECK(bc250_uma_restore(&io, expected, original, &result) == BC250_UMA_OK);
        CHECK(!memcmp(memory, original, 28));
        count = writes;
        CHECK(bc250_uma_restore(&io, original, original, &result) == BC250_UMA_NO_CHANGE);
        CHECK(writes == count);
        for (i = 6; i < 26; ++i) CHECK(!io.write(io.context, i, 0));
        CHECK(!io.write(io.context, 28, 0) && !io.write(io.context, ~0u, 0));
        CHECK(!io.read(io.context, 28, &value) && !io.read(io.context, ~0u, &value));
        CHECK(writes == count);
        for (i = 1; i <= 58; ++i) {
            init(map); fail_read = i;
            CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_READ_FAILED);
            CHECK(writes == 0);
            for (count = 0; count < 28; ++count) CHECK(block[count] == 0);
        }
        for (count = 1; count <= 29; ++count) if (count != 28) {
            init(map); fail_read = 1; bad_count = count;
            CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_READ_FAILED);
        }
        init(map); fail_read = 3; bad_count = 2;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_READ_FAILED);
        init(map); corrupt_read = 2;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_UNSTABLE);
        init(map); wrong_byte_view = 1;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_UNSTABLE);
        for (i = 1; i <= 6; ++i) {
            init(map); memcpy(original, memory, 28); fail_write = i;
            CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_RESTORED);
            CHECK(!memcmp(memory, original, 28) && result.operation_failed);
        }
        init(map); memcpy(original, memory, 28); bad_count = 2; fail_write = 2;
        CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_RESTORED);
        init(map); memcpy(original, memory, 28); corrupt_read = 65;
        CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_RESTORED);
        CHECK(!memcmp(memory, original, 28));
        init(map); memcpy(original, memory, 28); fail_all_writes = 1;
        CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
        CHECK(result.operation_failed && writes == 2);
        init(map); irql = 2;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_READ_FAILED);
        CHECK(!io.write(io.context, 0, 0) && calls == 0);
    }
    for (map = 0; map <= 3; map += 3) {
        init(1); transport.mapping = map;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_INVALID);
        CHECK(!io.read(io.context, 0, &value) && !io.write(io.context, 0, 0));
        CHECK(calls == 0);
    }
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
