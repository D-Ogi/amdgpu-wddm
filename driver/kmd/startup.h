#pragma once
#include "bc250kmd_escape.h"
#include "../shim/include/bc250_clock.h"

// Phase bits record attempts before entering code that can acquire GPU ownership.
#define BC250_START_GART 1u
#define BC250_START_PSP  2u
#define BC250_START_IH   4u
#define BC250_START_GFX  8u
#define BC250_START_CLOCK 16u

typedef struct _BC250_START_REPORT {
    ULONG Attempted;
    ULONG Completed;
    NTSTATUS Status;
    BOOLEAN InterruptConnected; // synchronization callback ran; not hardware delivery evidence
    BOOLEAN Ready;
    BOOLEAN Unwound;
    BOOLEAN StopUnconfirmed;
    struct bc250_clock_report Clock;
    BC250_ESCAPE_GART Gart;
    BC250_ESCAPE_PSP Psp;
    BC250_ESCAPE_IH Ih;
    BC250_ESCAPE_GFX Gfx;
} BC250_START_REPORT;

// Caller allocates this large report in NONPAGED pool before entering. Retain it
// for diagnostics, then release it. Only for an unpublished, exclusively owned
// PnP start: no diagnostic escape, admission or stop may race this call.
// WddmStart calls this before publishing its state; runtime ordering needs lab validation.
NTSTATUS GpuStartupInitialize(BC250_DEVICE* Device, BC250_START_REPORT* Report);
