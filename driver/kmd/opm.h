// The kernel state of the OPM interface (opm.c, BD-410 stage 1) and of the DxgkDdiQueryInterface log
// (query_interface.c). The rules are in opm_policy.h and query_interface_policy.h.
#pragma once
#include "opm_policy.h"
#include "query_interface_policy.h"

typedef struct _BC250_OPM {
    FAST_MUTEX Lock;        // PASSIVE_LEVEL: every OPM DDI; initialized in AddDevice
    OPM_OUTPUTS Outputs;    // the protected outputs dxgkrnl holds
    ULONG Logged;           // bit N: the first call of OPM_LOG_* N is logged; later calls are counted only
} BC250_OPM;

typedef struct _BC250_QUERY_LOG {
    FAST_MUTEX Lock;        // PASSIVE_LEVEL; initialized in AddDevice
    QI_LOG Log;
} BC250_QUERY_LOG;
