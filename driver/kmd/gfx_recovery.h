#ifndef BC250_GFX_RECOVERY_H
#define BC250_GFX_RECOVERY_H
// Include after bc250kmd.h. Internal startup API, not an escape or WDDM ABI.
#include "bc250_sdma.h"
#define BC250_SDMA0_RECOVERY_ADMISSION 1u
#define BC250_SDMA0_RECOVERY_RESET 2u
#define BC250_SDMA0_RECOVERY_PROBE 3u
#define BC250_SDMA0_RECOVERY_READY 4u
typedef struct _BC250_SDMA0_RECOVERY_REPORT {
    ULONG Stage;
    NTSTATUS Status;
    LONG ShimResult;
    struct bc250_sdma_reset_receipt Reset;
    ULONG PriorPagingSeq, PriorPagingInFlight, PriorPagingFailed, PriorRingOwes;
    ULONG ProbeSequence, Polls;
    ULONGLONG ExpectedContent, ObservedContent, ObservedFence, ProbeRptr, ProbeWptr;
    BOOLEAN Ready;
} BC250_SDMA0_RECOVERY_REPORT;
// PASSIVE_LEVEL, unpublished full-WDDM startup only (!Started && Wddm==NULL).
// All private owners/firmware/GART/IH and stage8 must already exist. No allocation
// or free occurs. Failure after admission retains backing and closes GFX access;
// the startup caller must enter its normal verified stop/unwind path.
NTSTATUS GfxRecoverSdma0Unpublished(BC250_DEVICE* Device, BC250_SDMA0_RECOVERY_REPORT* Report);
#endif
