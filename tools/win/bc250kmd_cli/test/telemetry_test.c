// Host test of the monitor's telemetry exports (Bc250Dpm, Bc250VideoMemory). test-telemetry.ps1 pastes the real
// block from bc250kmd_cli.c where ACTUAL_TELEMETRY stands; the three adapter-access helpers below replace the
// D3DKMT calls, so nothing here opens an adapter or sends an escape.
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((LONG)(s)>=0)
#endif
#define BC250_CONTROL_API
#define BC250_DEFAULT_HWID L"fixture"
// ACTUAL_TELEMETRY
static int escapeMode, escapeCalls, statsMode, statsCalls, adapterMode;
static unsigned escapeSize;
static BC250_ESCAPE_DPM sent;
static WCHAR adapterId[64];
static ULONG segmentCount = 3;
// The lab layout (driver/kmd/wddm.c WddmQuerySegment4): application local, aperture, table local.
static const struct { ULONG Aperture; ULONGLONG Resident, Committed, Limit; } g_Segments[10] = {
    { 0, 300ull << 20, 280ull << 20, 2048ull << 20 },
    { 1, 64ull << 20, 64ull << 20, 256ull << 20 },
    { 0, 16ull << 20, 16ull << 20, 32ull << 20 },
    { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1, 1 },
    { 1, 5, 5, ~0ull }, { 0, 7, 7, 9 },
};
static NTSTATUS TelemetryEscape(void *data, unsigned size)
{
    BC250_ESCAPE_DPM *d = data;
    escapeCalls++; escapeSize = size; sent = *d;
    if (escapeMode == 1) return (NTSTATUS)0xC00000A3;          // a KMD before the DPM escape
    if (escapeMode == 2) return 0;                             // untouched: UNKNOWN_COMMAND
    d->Status = BC250_ESCAPE_STATUS_DONE; d->Version = 0x000700B1u;
    d->Flags = BC250_DPM_FLAG_TEMPERATURE | BC250_DPM_FLAG_CLOCK | BC250_DPM_FLAG_HW_BUSY;
    d->TemperatureMc = 67500; d->ObservedMHz = 1000; d->BusyPermille = 910; d->BusyAvgPermille = 880;
    if (escapeMode == 3) { d->Status = BC250_ESCAPE_STATUS_REFUSED; d->NtStatus = 0xC000000Du; }
    if (escapeMode == 4) { d->Status = BC250_ESCAPE_STATUS_REFUSED; d->NtStatus = 0; }
    if (escapeMode == 5) d->AbiVersion = 2;
    if (escapeMode == 6) d->Command = 21;
    return 0;
}
static NTSTATUS TelemetryAdapter(const WCHAR *wantedId, LUID *luid, ULONGLONG *dedicated)
{
    wcsncpy_s(adapterId, 64, wantedId, _TRUNCATE);
    if (adapterMode == 1) return (NTSTATUS)0xC000000E;
    luid->LowPart = 0x032FD8C0u; luid->HighPart = 1;
    if (adapterMode != 2) *dedicated = 2080ull << 20;         // mode 2: GETSEGMENTSIZE refused, stays 0
    return 0;
}
static NTSTATUS TelemetryStatistics(D3DKMT_QUERYSTATISTICS *query)
{
    statsCalls++;
    if (query->AdapterLuid.LowPart != 0x032FD8C0u || query->AdapterLuid.HighPart != 1) return (NTSTATUS)0xC000000D;
    if (query->Type == D3DKMT_QUERYSTATISTICS_ADAPTER) {
        if (statsMode == 1) return (NTSTATUS)0xC0000001;
        query->QueryResult.AdapterInformation.NbSegments = segmentCount;
        return 0;
    }
    if (query->Type != D3DKMT_QUERYSTATISTICS_SEGMENT || query->QuerySegment.SegmentId >= 10) return (NTSTATUS)0xC000000D;
    if (statsMode == 2 && query->QuerySegment.SegmentId == 1) return (NTSTATUS)0xC0000001;
    query->QueryResult.SegmentInformation.Aperture = g_Segments[query->QuerySegment.SegmentId].Aperture;
    query->QueryResult.SegmentInformation.BytesResident = g_Segments[query->QuerySegment.SegmentId].Resident;
    query->QueryResult.SegmentInformation.BytesCommitted = g_Segments[query->QuerySegment.SegmentId].Committed;
    query->QueryResult.SegmentInformation.CommitLimit = g_Segments[query->QuerySegment.SegmentId].Limit;
    return 0;
}
static int checks, failures;
static void Check(int ok, int line, const char *text) { checks++; if (!ok) { failures++; printf("FAIL line %d: %s\n", line, text); } }
#define CHECK(x) Check((x), __LINE__, #x)
int main(void)
{
    BC250_ESCAPE_DPM d;
    BC250_VIDEO_MEMORY m;

    /* The deployed callers pass BC250_DPM_ABI1_SIZE, the layout they were built against; RUN_DPM itself grew
     * to 192 bytes with the idle state (0.7.207) and the driver takes either size with its own AbiVersion. */
    CHECK(BC250_DPM_ABI1_SIZE == 160 && sizeof(d) >= 160 && sizeof(m) == 264);
    CHECK(Bc250Dpm(NULL, BC250_DPM_ABI1_SIZE) < 0 && Bc250Dpm(&d, 159) < 0 && escapeCalls == 0);
    CHECK(Bc250Dpm(&d, 161) < 0 && escapeCalls == 0);
    CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == 0 && escapeCalls == 1 && escapeSize == BC250_DPM_ABI1_SIZE);
    CHECK(sent.Magic == BC250_ESCAPE_MAGIC && sent.Command == 23 && sent.AbiVersion == BC250_DPM_ABI_1 && sent.Op == 0);
    CHECK(sent.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND && sent.SubmitBusyPermille == 0 && sent.SdmaBusyPermille == 0);
    CHECK(sent.ExpectedGeneration == 0 && sent.Flags == 0);
    CHECK(d.TemperatureMc == 67500 && d.ObservedMHz == 1000 && d.BusyAvgPermille == 880);
    escapeMode = 1; CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == (LONG)0xC00000A3);
    escapeMode = 2; CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == (LONG)0xC00000BB);
    escapeMode = 3; CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == (LONG)0xC000000D);
    escapeMode = 4; CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == (LONG)0xC00000A3);
    escapeMode = 5; CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == (LONG)0xC000000D);
    escapeMode = 6; CHECK(Bc250Dpm(&d, BC250_DPM_ABI1_SIZE) == (LONG)0xC000000D);
    escapeMode = 0;
    /* A caller rebuilt against this header asks with the whole structure, and then the ABI is 2. */
    escapeCalls = 0;
    CHECK(Bc250Dpm(&d, (ULONG)sizeof(d)) == 0 && escapeCalls == 1 && escapeSize == sizeof(d));
    CHECK(sent.AbiVersion == BC250_DPM_ABI && sent.Command == 23 && sent.Op == 0);
    escapeMode = 5; CHECK(Bc250Dpm(&d, (ULONG)sizeof(d)) == 0);   /* the fixture answers 2, which is the request */
    escapeMode = 0;

    CHECK(Bc250VideoMemory(NULL, NULL, sizeof(m)) < 0 && Bc250VideoMemory(NULL, &m, 263) < 0 && statsCalls == 0);
    CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == 0 && !wcscmp(adapterId, L"fixture"));
    CHECK(m.Size == 264 && m.Segments == 3 && m.ApertureMask == 2 && m.LuidLow == 0x032FD8C0u && m.LuidHigh == 1);
    CHECK(m.LocalResident == (316ull << 20) && m.LocalCommitted == (296ull << 20) && m.LocalLimit == (2080ull << 20));
    CHECK(m.ApertureResident == (64ull << 20) && m.ApertureLimit == (256ull << 20) && m.DedicatedVideoMemory == (2080ull << 20));
    CHECK(m.Resident[0] == (300ull << 20) && m.Limit[1] == (256ull << 20) && m.Committed[2] == (16ull << 20) && m.Resident[3] == 0);
    CHECK(Bc250VideoMemory(L"", &m, sizeof(m)) == 0 && !wcscmp(adapterId, L"fixture"));
    CHECK(Bc250VideoMemory(L"PCI\\VEN_10DE", &m, sizeof(m)) == 0 && !wcscmp(adapterId, L"PCI\\VEN_10DE"));
    segmentCount = 10;
    CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == 0 && m.Segments == 10 && m.ApertureMask == 2);
    CHECK(m.ApertureLimit == ~0ull && m.ApertureResident == (64ull << 20) + 5);   // saturates, never wraps
    CHECK(m.LocalResident == (316ull << 20) + 5 + 7 && m.LocalLimit == (2080ull << 20) + 5 + 9);
    segmentCount = 33;
    CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == (LONG)0xC000000D);
    segmentCount = 3;
    adapterMode = 1; CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == (LONG)0xC000000E);
    adapterMode = 2; CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == 0 && m.DedicatedVideoMemory == 0 && m.LocalLimit == (2080ull << 20));
    adapterMode = 0;
    statsMode = 1; CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == (LONG)0xC0000001);
    statsMode = 2; CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == (LONG)0xC0000001);
    statsMode = 0;
    printf("Telemetry exports: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
