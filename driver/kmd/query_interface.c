// DxgkDdiQueryInterface of the full table (BD-410). The one place that decides which interface this adapter gives
// to dxgkrnl and to other kernel clients. Today that is GUID_DEVINTERFACE_OPM version 1 (opm.c); every other
// request is answered with STATUS_NOT_SUPPORTED, which the DDI requires for an interface the miniport does not
// support and which "informs the operating system to pass the query on to the next driver"
// (nc-dispmprt-dxgkddi_query_interface.md:59). A new interface gets its own module and one line here.
// The display-only table has no QueryInterface member set: it has no OPM, and nothing else is offered.
#include "bc250kmd.h"

void QueryInterfaceInitialize(BC250_DEVICE* Device)
{
    ExInitializeFastMutex(&Device->QueryLog.Lock);
}

// The log line of a request (query_interface_policy.h): the first request of each GUID, size, version and answer,
// then one line when QI_LOG_KINDS kinds are kept. Both flavours.
static void QueryInterfaceLog(BC250_DEVICE* Device, const QUERY_INTERFACE* Query, NTSTATUS Status)
{
    static const UCHAR none[16] = {0};
    const GUID* g = Query->InterfaceType;
    const UCHAR* b;
    ULONG requests;
    int what;

    ExAcquireFastMutex(&Device->QueryLog.Lock);
    what = QiLogNote(&Device->QueryLog.Log, g, Query->Size, Query->Version, Status);
    requests = Device->QueryLog.Log.Requests;
    ExReleaseFastMutex(&Device->QueryLog.Lock);
    if (what == QI_LOG_LINE) {
        b = g ? g->Data4 : none;
        // "qi: {GUID} s <Size> v <Version> -> <status> #<request number>", within the 159 characters of a line.
        GuardLog("qi: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} s %u v %u -> 0x%08lX #%lu",
            g ? g->Data1 : 0ul, (UINT)(g ? g->Data2 : 0), (UINT)(g ? g->Data3 : 0), b[0], b[1], b[2], b[3], b[4],
            b[5], b[6], b[7], (UINT)Query->Size, (UINT)Query->Version, (ULONG)Status, requests);
    } else if (what == QI_LOG_OVERFLOW) {
        GuardLog("qi: %u kinds logged, later kinds are not logged (request %lu)", QI_LOG_KINDS, requests);
    }
}

NTSTATUS Bc250QueryInterface(IN_CONST_PVOID MiniportDeviceContext, IN_PQUERY_INTERFACE QueryInterface)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    NTSTATUS status;

    if (device == NULL || QueryInterface == NULL) return STATUS_NOT_SUPPORTED;
    status = OpmQueryInterface(device, QueryInterface);
    QueryInterfaceLog(device, QueryInterface, status);
    return status;
}
