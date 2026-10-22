// Fixed-size lab-only BD-223 request. Result addresses are output, never input.
#pragma once
#include <stddef.h>
#include "bc250kmd_escape.h"
#include "dcn_latch_probe.h"
// ABI 3 appends QPC frequency and separate normal/cleanup unlock observations.
#define BC250_LATCH_PROBE_ABI 3u
typedef struct _BC250_ESCAPE_LATCH_PROBE {
    unsigned long Magic, Command, Status, Version;
    unsigned long AbiVersion, Arm, ExpectedVersion, NtStatus;
    unsigned long Reserved[8];
    BC250_LATCH_PROBE_RESULT Result;
} BC250_ESCAPE_LATCH_PROBE;
typedef char BC250_LATCH_SAMPLE_SIZE_CHECK[(sizeof(BC250_LATCH_SAMPLE)==64)?1:-1];
typedef char BC250_LATCH_UNLOCK_SIZE_CHECK[(sizeof(BC250_LATCH_UNLOCK_SAMPLE)==48)?1:-1];
typedef char BC250_LATCH_RESULT_SIZE_CHECK[(sizeof(BC250_LATCH_PROBE_RESULT)==416)?1:-1];
typedef char BC250_LATCH_ESCAPE_SIZE_CHECK[(sizeof(BC250_ESCAPE_LATCH_PROBE)==480)?1:-1];
// Raw-DWORD decoder offsets must agree on both Windows client architectures.
typedef char BC250_LATCH_VALID_OFFSET[(offsetof(BC250_LATCH_SAMPLE,ValidMask)==40)?1:-1];
typedef char BC250_LATCH_PHYSICAL_OFFSET[(offsetof(BC250_LATCH_SAMPLE,Physical)==56)?1:-1];
typedef char BC250_LATCH_BEFORE_OFFSET[(offsetof(BC250_ESCAPE_LATCH_PROBE,Result)+
    offsetof(BC250_LATCH_PROBE_RESULT,Before)==120)?1:-1];
typedef char BC250_LATCH_UNLOCK_OFFSET[(offsetof(BC250_ESCAPE_LATCH_PROBE,Result)+
    offsetof(BC250_LATCH_PROBE_RESULT,Unlock)==384)?1:-1];
