// Fixed-size lab-only BD-223 request. Result addresses are output, never input.
#pragma once
#include "bc250kmd_escape.h"
#include "dcn_latch_probe.h"
#define BC250_LATCH_PROBE_ABI 1u
typedef struct _BC250_ESCAPE_LATCH_PROBE {
    unsigned long Magic, Command, Status, Version;
    unsigned long AbiVersion, Arm, ExpectedVersion, NtStatus;
    unsigned long Reserved[8];
    BC250_LATCH_PROBE_RESULT Result;
} BC250_ESCAPE_LATCH_PROBE;
typedef char BC250_LATCH_SAMPLE_SIZE_CHECK[(sizeof(BC250_LATCH_SAMPLE)==64)?1:-1];
typedef char BC250_LATCH_RESULT_SIZE_CHECK[(sizeof(BC250_LATCH_PROBE_RESULT)==312)?1:-1];
typedef char BC250_LATCH_ESCAPE_SIZE_CHECK[(sizeof(BC250_ESCAPE_LATCH_PROBE)==376)?1:-1];
