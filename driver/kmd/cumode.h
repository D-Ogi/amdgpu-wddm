// CU mode state of one adapter (cumode.c, docs/design/cu-mode.md). The policy and the register
// sequence are driver/shim/bc250_cu_mode.c; this is what the miniport keeps around them.
#pragma once
#include "bc250_cu_mode.h"

// amdgpu's kernel log on unit A (E03 dmesg): "SE 2, SH per SE 2, CU per SH 10". gfx.c hands the same
// three numbers to the shim; CU per SH is what makes 40 reachable at all (5 WGPs per shader array).
#define BC250_SHADER_ENGINES 2u
#define BC250_SH_PER_SE 2u
#define BC250_MAX_CU_PER_SH 10u
#define BC250_PCI_ID 0x13FE1002u    // configuration dword 0: device 13FE, vendor 1002

typedef struct _BC250_CU_MODE_STATE {
    KMUTEX Lock;                        // Prepare/Finish/Confirm: PASSIVE_LEVEL registry transitions
    KSPIN_LOCK SnapLock;                // Snapshot and Info, read by the escape and QueryAdapterInfo
    struct bc250_cu_request Request;
    struct bc250_cu_decision Decision;
    // The hook's context: filled before the constants stage, read after it. Stays installed for the
    // whole device start, so a retained-power resume re-runs the stage with the mode that stuck.
    struct bc250_cu_mode_hw Hw;
    BOOLEAN Installed;                  // Hw is adev->gfx.cu_mode_ctx
    BOOLEAN StockRecord;                // this boot's stock came from the volatile record
    BOOLEAN Pending;                    // Pending is durable on disk for this start's 40
    BOOLEAN Confirmed;
    ULONG PciId;                        // vendor | device << 16, from configuration space
    ULONGLONG Generation;               // start-health generation of this start
    // Under SnapLock. Snap is the hook's last run (the start's, then every retained-power resume's);
    // Valid, Applied, Reason and Info are the start's, published at Finish, and the caps follow Info.
    struct bc250_cu_mode_hw Snap;
    ULONG Runs;
    BOOLEAN Valid;
    ULONG Requested, Applied, Reason;
    struct bc250_cu_info Info;
} BC250_CU_MODE_STATE;
