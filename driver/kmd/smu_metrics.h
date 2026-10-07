// The SMU metrics table as the miniport keeps it (smu_metrics.c, docs/design/dpm.md "Power reading"): the page the
// firmware writes into, the gate, the reader and the published snapshot. The rules are driver/shim/bc250_smu_metrics.c,
// which has no Windows in it; the messages go through the one SMU owner (smu.c SmuReadMetrics).
#pragma once
#include "bc250_smu_metrics.h"
#include "bc250kmd_escape.h"

// Where the page lives: a carve-out page below the PSP's fence page (end - 0x17000, psp.c) and above the last 64 KB,
// which hold the IP discovery table. It is in the top 2 MB, which Windows' memory segment never covers, so the SMU's
// write can reach nothing but this page. The number is in the reservation table of bc250kmd.h.
C_ASSERT(BC250_SMU_METRICS_BYTES <= BC250_SMU_METRICS_PAGE);
C_ASSERT(BC250_DPM_CPU_CLOCKS == BC250_SMU_METRICS_CPU_CORES && RTL_NUMBER_OF(((BC250_DPM_CLOCKS*)0)->L3MHz) ==
         BC250_SMU_METRICS_L3);
C_ASSERT(BC250_DPM_METRICS_OFF == BC250_SMU_METRICS_STATE_OFF && BC250_DPM_METRICS_WAITING == BC250_SMU_METRICS_STATE_WAITING &&
         BC250_DPM_METRICS_OK == BC250_SMU_METRICS_STATE_OK && BC250_DPM_METRICS_REFUSED == BC250_SMU_METRICS_STATE_REFUSED &&
         BC250_DPM_METRICS_NO_TABLE == BC250_SMU_METRICS_STATE_NO_TABLE &&
         BC250_DPM_METRICS_BAD_TABLE == BC250_SMU_METRICS_STATE_BAD_TABLE);

typedef struct _BC250_SMU_METRICS_SNAP {
    ULONG State;                            // enum bc250_smu_metrics_state
    ULONG Reads, Failures;
    LONG LastStatus;                        // the transport's last result
    ULONG LastParse;                        // enum bc250_smu_metrics_parse of the last table the firmware wrote
    BOOLEAN HasTable;
    ULONGLONG TableAt;                      // KeQueryInterruptTime of the last accepted table, 0 for none
    struct bc250_smu_metrics Table;         // the last accepted table
} BC250_SMU_METRICS_SNAP;

typedef struct _BC250_SMU_METRICS {
    KSPIN_LOCK SnapLock;                    // Snap, written by the governor thread and read by the escape
    BOOLEAN Enabled;                        // EnableSmuMetrics at this start
    volatile ULONG* Page;                   // the uncached mapping of the page, NULL when there is none
    PHYSICAL_ADDRESS Physical;
    ULONGLONG TableMc;                      // the page's GPU (MC) address, the one the firmware is told
    BOOLEAN LoggedEnd;                      // the line that says why the reader stopped is written once a start
    struct bc250_smu_metrics_reader Reader; // the governor thread's alone while it runs
    BC250_SMU_METRICS_SNAP Snap;            // under SnapLock
} BC250_SMU_METRICS;
// The functions are declared in bc250kmd.h, beside the hardware monitor's.
