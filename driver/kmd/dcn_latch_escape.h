// Fixed-size lab-only BD-223 request. Result addresses are output, never input.
#pragma once
#include <stddef.h>
#include "bc250kmd_escape.h"
#include "dcn_latch_probe.h"
// ABI 2 adds a raw flip-control word inside each sample's brackets. Size stays
// 376 bytes, but derived fields move: ABI 1 must never be decoded as ABI 2.
#define BC250_LATCH_PROBE_ABI 2u
typedef struct _BC250_ESCAPE_LATCH_PROBE {
    unsigned long Magic, Command, Status, Version;
    unsigned long AbiVersion, Arm, ExpectedVersion, NtStatus;
    unsigned long Reserved[8];
    BC250_LATCH_PROBE_RESULT Result;
} BC250_ESCAPE_LATCH_PROBE;
typedef char BC250_LATCH_SAMPLE_SIZE_CHECK[(sizeof(BC250_LATCH_SAMPLE)==64)?1:-1];
typedef char BC250_LATCH_RESULT_SIZE_CHECK[(sizeof(BC250_LATCH_PROBE_RESULT)==312)?1:-1];
typedef char BC250_LATCH_ESCAPE_SIZE_CHECK[(sizeof(BC250_ESCAPE_LATCH_PROBE)==376)?1:-1];
// Raw-DWORD decoder offsets must agree on both Windows client architectures.
typedef char BC250_LATCH_VALID_OFFSET[(offsetof(BC250_LATCH_SAMPLE,ValidMask)==40)?1:-1];
typedef char BC250_LATCH_PHYSICAL_OFFSET[(offsetof(BC250_LATCH_SAMPLE,Physical)==56)?1:-1];
typedef char BC250_LATCH_BEFORE_OFFSET[(offsetof(BC250_ESCAPE_LATCH_PROBE,Result)+
    offsetof(BC250_LATCH_PROBE_RESULT,Before)==120)?1:-1];
