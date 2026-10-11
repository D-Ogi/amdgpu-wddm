// Production adapter over an indexed extended-CMOS fixture; no hardware I/O.
#include <stdio.h>
#include <string.h>
#include <ntddk.h>
#include "uma_transport.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
static unsigned char memory[28];
static unsigned calls, reads, writes, index_value, selected, irql;
static unsigned drop_write, drop_all, corrupt_read;
unsigned char KeGetCurrentIrql(void) { return (unsigned char)irql; }
void WRITE_PORT_UCHAR(PUCHAR port, unsigned char value)
{
    ++calls;
    CHECK(irql == 0);
    if ((ULONG_PTR)port == 0x72) {
        CHECK(!selected && value >= 0x90 && value <= 0xab);
        index_value = value; selected = 1;
        return;
    }
    CHECK((ULONG_PTR)port == 0x73 && selected);
    if ((ULONG_PTR)port != 0x73 || !selected) return;
    selected = 0; ++writes;
    CHECK(index_value < 0x96 || index_value >= 0xaa);
    if (index_value < 0x90 || index_value > 0xab) return;
    if (!drop_all && writes != drop_write) memory[index_value - 0x90] = value;
}
unsigned char READ_PORT_UCHAR(PUCHAR port)
{
    unsigned char value;
    ++calls;
    CHECK(irql == 0 && (ULONG_PTR)port == 0x73 && selected);
    if ((ULONG_PTR)port != 0x73 || !selected || index_value < 0x90 || index_value > 0xab) return 0xff;
    selected = 0; ++reads;
    value = memory[index_value - 0x90];
    return reads == corrupt_read ? (unsigned char)(value ^ 1) : value;
}
static void init(void)
{
    unsigned i, sum = 0;
    calls = reads = writes = selected = irql = drop_write = drop_all = corrupt_read = 0;
    memcpy(memory, "CMSB", 4);
    for (i = 6; i < 26; ++i) memory[i] = (unsigned char)(i * 7);
    memory[26] = 0; memory[27] = 0x20;
    for (i = 6; i < 28; ++i) sum += memory[i];
    memory[4] = (unsigned char)sum; memory[5] = (unsigned char)(sum >> 8);
}
int main(void)
{
    struct bc250_uma_transport transport = {BC250_UMA_MAPPING_EXTENDED_PORT};
    struct bc250_uma_io io;
    struct bc250_uma_result result;
    unsigned char block[28], original[28], expected[28], value;
    unsigned i, count, successful_writes;
    bc250_uma_transport_io(&transport, &io);
    init(); memcpy(original, memory, 28);
    CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_OK);
    CHECK(reads == 112 && writes == 0 && !memcmp(block, original, 28));
    CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_OK);
    successful_writes = writes;
    CHECK(memory[27] == 0x30 && !memcmp(memory + 6, original + 6, 20));
    memcpy(expected, memory, 28);
    CHECK(bc250_uma_restore(&io, expected, original, &result) == BC250_UMA_OK);
    CHECK(!memcmp(memory, original, 28));
    count = writes;
    CHECK(bc250_uma_restore(&io, original, original, &result) == BC250_UMA_NO_CHANGE);
    CHECK(writes == count);
    count = calls;
    for (i = 6; i < 26; ++i) CHECK(!io.write(io.context, i, 0));
    CHECK(!io.write(io.context, 28, 0) && !io.write(io.context, ~0u, 0));
    CHECK(!io.read(io.context, 28, &value) && !io.read(io.context, ~0u, &value));
    CHECK(calls == count);
    for (i = 1; i <= 112; ++i) {
        init(); corrupt_read = i;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_UNSTABLE);
        CHECK(writes == 0);
        for (count = 0; count < 28; ++count) CHECK(block[count] == 0);
    }
    for (i = 1; i <= successful_writes; ++i) {
        init(); memcpy(original, memory, 28); drop_write = i;
        CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_RESTORED);
        CHECK(!memcmp(memory, original, 28) && result.operation_failed);
    }
    init(); memcpy(original, memory, 28); drop_all = 1;
    CHECK(bc250_uma_apply(&io, original, 12288, &result) == BC250_UMA_ROLLBACK_UNCONFIRMED);
    CHECK(result.operation_failed && writes == 2);
    init(); irql = 2;
    CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_READ_FAILED);
    CHECK(!io.write(io.context, 0, 0) && calls == 0);
    for (i = 0; i <= 4; ++i) if (i != BC250_UMA_MAPPING_EXTENDED_PORT) {
        init(); transport.mapping = i;
        CHECK(bc250_uma_transport_read_block(&transport, block) == BC250_UMA_INVALID);
        CHECK(!io.read(io.context, 0, &value) && !io.write(io.context, 0, 0));
        CHECK(calls == 0);
    }
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
