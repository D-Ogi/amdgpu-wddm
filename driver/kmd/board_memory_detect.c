#include <ntddk.h>
#include <string.h>
#include "board_memory_detect.h"

static int rtc_read(unsigned mapping, unsigned index, unsigned char* value)
{
    ULONG count;
    unsigned char temporary = 0;
    /* Fixed read-only RTC selection; never read interrupt-status register C. */
    if (index != 0 && index != 0x0a && index != 0x0b && index != 0x0d) return 0;
#pragma warning(push)
#pragma warning(disable:4996)
    count = HalGetBusDataByOffset(Cmos, 0,
        mapping == BC250_UMA_MAPPING_SLOT ? index : 0, &temporary,
        mapping == BC250_UMA_MAPPING_OFFSET ? index : 0, 1);
#pragma warning(pop)
    if (count != 1) return 0;
    *value = temporary;
    return 1;
}
static int rtc_second(unsigned mapping, unsigned* second, unsigned* mode)
{
    unsigned char a, b, d, value, again, after;
    if (!rtc_read(mapping, 0x0a, &a) || (a & 0x80) ||
        !rtc_read(mapping, 0x0b, &b) || (b & 0x80) ||
        !rtc_read(mapping, 0x0d, &d) || !(d & 0x80) ||
        !rtc_read(mapping, 0, &value) || !rtc_read(mapping, 0, &again) ||
        value != again || !rtc_read(mapping, 0x0a, &after) || (after & 0x80)) return 0;
    *mode = b & 4;
    if (!(b & 4)) {
        if ((value & 15) > 9 || (value >> 4) > 5) return 0;
        value = (unsigned char)((value >> 4) * 10 + (value & 15));
    }
    if (value > 59) return 0;
    *second = value;
    return 1;
}
static int rtc_progress(unsigned mapping)
{
    unsigned first = 0, mode = 0, have = 0, i;
    LARGE_INTEGER delay;
    delay.QuadPart = -1250000; /* Relative 125 ms, at most 12 waits per mapping. */
    for (i = 0; i <= 12; ++i) {
        unsigned current, currentMode;
        if (rtc_second(mapping, &current, &currentMode)) {
            if (!have) { first = current; mode = currentMode; have = 1; }
            else if (currentMode != mode) return 0;
            else if (current == (first + 1) % 60) return 1;
            else if (current != first) return 0;
        }
        if (i != 12 && !NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &delay))) return 0;
    }
    return 0;
}
int BoardMemoryDetect(struct bc250_uma_transport* transport, unsigned char* block)
{
    unsigned mapping, selected = 0, passes = 0;
    unsigned char candidate[BC250_UMA_BLOCK_BYTES], saved[BC250_UMA_BLOCK_BYTES];
    if (transport) transport->mapping = BC250_UMA_MAPPING_NONE;
    if (block) memset(block, 0, BC250_UMA_BLOCK_BYTES);
    if (!transport || !block || KeGetCurrentIrql() != PASSIVE_LEVEL) return 0;
    for (mapping = BC250_UMA_MAPPING_SLOT; mapping <= BC250_UMA_MAPPING_OFFSET; ++mapping) {
        struct bc250_uma_transport attempt;
        unsigned mib;
        attempt.mapping = mapping;
        if (bc250_uma_transport_read_block(&attempt, candidate) != BC250_UMA_OK ||
            !bc250_uma_validate(candidate)) continue;
        mib = (unsigned)candidate[26] | ((unsigned)candidate[27] << 8);
        if (mib != 8192 && mib != 12288) continue;
        if (!rtc_progress(mapping)) continue;
        /* Require the same block after the RTC interval too. */
        if (bc250_uma_transport_read_block(&attempt, saved) != BC250_UMA_OK ||
            memcmp(candidate, saved, sizeof(saved))) continue;
        ++passes;
        selected = mapping;
        memcpy(block, saved, sizeof(saved));
    }
    if (passes != 1) { memset(block, 0, BC250_UMA_BLOCK_BYTES); return 0; }
    transport->mapping = selected;
    return 1;
}
