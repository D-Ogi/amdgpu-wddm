// Host test of the monitor's and the application's exports (Bc250Dpm, Bc250VideoMemory, from 0.7.210
// Bc250DpmCurve and Bc250Cpu, and Bc250Fan for the case fan control). test-telemetry.ps1 pastes the real block from bc250kmd_cli.c where the marker
// below stands; the adapter-access helpers below replace the D3DKMT calls, so nothing here opens an
// adapter or sends an escape. What the two new exports must get right and this test holds them to: the escape
// flag per operation (a software snapshot for the curve and for a CPU READ, HardwareAccess for a CPU write), the
// generation a write carries, and the refusal of a malformed request before any escape is sent.
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
static int escapeMode, escapeCalls, statsMode, statsCalls, adapterMode, curveMode, cpuMode;
static int escapeHardware = -1;         // the flag the last escape asked for: 0 software, 1 HardwareAccess
static unsigned escapeSize;
static BC250_ESCAPE_DPM sent;
static BC250_ESCAPE_DPM_CURVE sentCurve;
static BC250_ESCAPE_CPU sentCpu;
static BC250_ESCAPE_FAN sentFan;
static int fanMode;
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
// The curve and the CPU surface, answered like the firmware would: the request as it arrived is kept for the
// assertions, and the reply carries the fields the window reads.
static NTSTATUS TelemetryCurve(BC250_ESCAPE_DPM_CURVE *c)
{
    unsigned i;
    sentCurve = *c;
    if (curveMode == 1) return (NTSTATUS)0xC00000A3;            // a KMD before 0.7.210
    if (curveMode == 2) return 0;                               // untouched: UNKNOWN_COMMAND
    c->Status = BC250_ESCAPE_STATUS_DONE; c->Version = 0x000700D2u;
    c->Flags = BC250_DPM_CURVE_FLAG_VALID | BC250_DPM_CURVE_FLAG_GOVERNING;
    c->FirstMHz = 1000; c->StepMHz = 100; c->Points = BC250_DPM_CURVE_POINTS;
    for (i = 0; i < BC250_DPM_CURVE_POINTS; i++) {
        c->ActiveMv[i] = 820 + i * 10; c->DefaultMv[i] = 820 + i * 18; c->FloorMv[i] = 820 + i * 8;
    }
    c->Serial = 3; c->Level = 5; c->LevelMHz = 1000; c->LevelMv = 820; c->TemperatureMc = 67500;
    if (curveMode == 3) { c->Status = BC250_ESCAPE_STATUS_REFUSED; c->NtStatus = 0xC000000Du; }
    if (curveMode == 4) { c->Status = BC250_ESCAPE_STATUS_REFUSED; c->NtStatus = 0; }
    if (curveMode == 5) c->AbiVersion = 2;
    if (curveMode == 6) c->Op = 9;
    return 0;
}
static NTSTATUS TelemetryCpu(BC250_ESCAPE_CPU *c)
{
    sentCpu = *c;
    if (cpuMode == 1) return (NTSTATUS)0xC00000A3;
    if (cpuMode == 2) return 0;
    c->Status = BC250_ESCAPE_STATUS_DONE; c->Version = 0x000700D2u;
    c->Flags = BC250_CPU_FLAG_VALID | BC250_CPU_FLAG_TUNE_ON | BC250_CPU_FLAG_QUEUE3_PROVEN;
    c->VoltageMv = 1050; c->CapC = 100; c->Cores = 6; c->Threads = 12; c->CoreMask = 0x77;
    c->BaselineMaxMHz = 3600; c->TemperatureMc = 61000;
    if (cpuMode == 3) { c->Status = BC250_ESCAPE_STATUS_REFUSED; c->NtStatus = 0xC0000061u; }
    if (cpuMode == 4) { c->Status = BC250_ESCAPE_STATUS_REFUSED; c->NtStatus = 0; }
    if (cpuMode == 5) c->Command = 21;
    /* A driver of 0.7.212 or older: a KEEP that does not carry HardwareAccess is refused at the gate with
       STATUS_INVALID_PARAMETER and the build in Version; every other operation is answered as above. */
    if (cpuMode == 6 && !escapeHardware && c->Op == BC250_CPU_OP_KEEP) {
        memset(c, 0, sizeof(*c));
        c->Status = BC250_ESCAPE_STATUS_REFUSED; c->NtStatus = 0xC000000Du; c->Version = 0x000700D4u;
    }
    return 0;
}
static NTSTATUS TelemetryFan(BC250_ESCAPE_FAN *f)
{
    sentFan = *f;
    if (fanMode == 1) return (NTSTATUS)0xC00000A3;
    if (fanMode == 2) return 0;                                 // untouched: UNKNOWN_COMMAND
    f->Status = BC250_ESCAPE_STATUS_DONE; f->Version = 0x000700D2u;
    f->Flags = BC250_FAN_FLAG_ENABLED | BC250_FAN_FLAG_CONTROLLING;
    f->State = 2; f->Rpm = 1180; f->Generation = 77;
    if (fanMode == 3) { f->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; f->NtStatus = 0xC0000022u; }
    if (fanMode == 4) { f->Status = BC250_ESCAPE_STATUS_REFUSED; f->NtStatus = 0; }
    if (fanMode == 5) f->AbiVersion = 2;
    if (fanMode == 6) f->Op = 9;
    if (fanMode == 7) { f->Error = 5; f->NtStatus = 0xC000000Du; }
    return 0;
}
// The same shape as the DLL: one function with the flag, and TelemetryEscape as its software-only wrapper, so
// that the test sees exactly which flag each export asked for.
static void UmaMockBlock(unsigned char *b, unsigned long mib)
{
 memset(b,0,28);b[0]=0x41;b[1]=0x50;b[2]=0x43;b[3]=0x42;
 b[26]=(unsigned char)mib;b[27]=(unsigned char)(mib>>8);b[4]=(unsigned char)(b[26]+b[27]);
}
static int umaMode, umaHardwareCalls;
static BC250_ESCAPE_BOARD_MEMORY sentUma;
static NTSTATUS TelemetryUma(BC250_ESCAPE_BOARD_MEMORY* u)
{
 sentUma=*u;if(escapeHardware)umaHardwareCalls++;
 if(u->Op==BC250_BOARD_MEMORY_OP_PROBE){
  BC250_ESCAPE_BOARD_MEMORY_PROBE* p=(BC250_ESCAPE_BOARD_MEMORY_PROBE*)u;
  if(umaMode==1)return 0;
  p->Status=BC250_ESCAPE_STATUS_DONE;p->Reason=BC250_BOARD_MEMORY_REASON_READ;p->Flags=BC250_BOARD_MEMORY_SUPPORTED;
  p->SlotCount=28;p->OffsetCount=0;p->SlotBlock[0]=0x24;p->RtcCount[3]=1;p->RtcValue[3]=0x80;
  if(umaMode==2){p->Status=BC250_ESCAPE_STATUS_REFUSED;p->NtStatus=0xC0000022u;}
  if(umaMode==3)p->Magic=0;
  if(umaMode==4)p->Reserved[2]=1;
  if(umaMode==5){p->Flags=0;p->Reason=BC250_BOARD_MEMORY_REASON_BOARD;p->Status=BC250_ESCAPE_STATUS_REFUSED;p->NtStatus=0xC00000BBu;}
  if(umaMode==6)p->Flags=0;
  return 0;
 }
 if(umaMode==1)return 0;
 if(umaMode==2)return (NTSTATUS)0xC00000A3;
 u->Status=BC250_ESCAPE_STATUS_DONE;u->Flags=BC250_BOARD_MEMORY_ACTIVE_VALID|BC250_BOARD_MEMORY_SUPPORTED;
 u->ProviderId=BC250_BOARD_MEMORY_PROVIDER_BC250_ABL;u->AllowedMiB[0]=8192;u->AllowedMiB[1]=12288;u->NeedsRestart=1;
 u->ActiveBytes=12ull<<30;u->Reason=BC250_BOARD_MEMORY_REASON_NO_TRANSPORT;
 if(umaMode==3)u->Magic^=1;
 if(umaMode==4)u->AbiVersion++;
 if(umaMode==5)u->Op=9;
 if(umaMode==6)u->Flags=32;
 if(umaMode==7)u->Reason=99;
 if(umaMode==8)u->Reserved[7]=1;
 if(umaMode==9){u->Status=BC250_ESCAPE_STATUS_REFUSED;u->NtStatus=0xC0000022u;}
 if(umaMode>=10 && umaMode<=13){u->Reason=BC250_BOARD_MEMORY_REASON_READY;u->Flags|=BC250_BOARD_MEMORY_READ_VALID|BC250_BOARD_MEMORY_WRITE_ALLOWED;u->RequestedMiB=u->Op==BC250_BOARD_MEMORY_OP_SET?sentUma.RequestedMiB:8192;UmaMockBlock(u->ObservedBlock,u->RequestedMiB);}
 if(umaMode==11){u->Flags|=BC250_BOARD_MEMORY_BACKUP_VALID;u->PreviousMiB=8192;}
 if(umaMode==12)u->Reason=BC250_BOARD_MEMORY_REASON_NO_TRANSPORT;
 if(umaMode==13)u->Flags&=~BC250_BOARD_MEMORY_READ_VALID;
 if(umaMode==14) {u->Flags=0;u->ProviderId=0;u->AllowedMiB[0]=u->AllowedMiB[1]=u->NeedsRestart=0;u->ActiveBytes=0;u->Reason=BC250_BOARD_MEMORY_REASON_BOARD;}
 if(umaMode==15)u->ProviderId=0;
 if(umaMode==16)u->ProviderId=2;
 if(umaMode==17)u->AllowedMiB[1]=14336;
 if(umaMode==18)u->NeedsRestart=0;
 if(umaMode==19)u->Flags=BC250_BOARD_MEMORY_READ_VALID|BC250_BOARD_MEMORY_WRITE_ALLOWED;
 if(umaMode==20)u->AbiVersion=1;
 if(umaMode==21)u->Reason=BC250_BOARD_MEMORY_REASON_BOARD;
 if(umaMode>=22 && umaMode<=27){
  u->Reason=BC250_BOARD_MEMORY_REASON_READY;u->Flags|=BC250_BOARD_MEMORY_READ_VALID|BC250_BOARD_MEMORY_WRITE_ALLOWED;
  u->RequestedMiB=u->Op==BC250_BOARD_MEMORY_OP_SET?sentUma.RequestedMiB:8192;
  UmaMockBlock(u->ObservedBlock,u->RequestedMiB);
  if(escapeHardware){
   if(umaMode==22)u->ResultCode=-5;
   if(umaMode==23)u->RequestedMiB=8192;
   if(umaMode==24)u->ObservedBlock[4]++;
   if(umaMode==25){u->ObservedBlock[6]=1;u->ObservedBlock[4]++;}
   if(umaMode==26){u->Flags&=~BC250_BOARD_MEMORY_WRITE_ALLOWED;u->Reason=BC250_BOARD_MEMORY_REASON_UNKNOWN_STATE;u->Status=BC250_ESCAPE_STATUS_REFUSED;u->NtStatus=0xC00000A3u;}
   if(umaMode==27)u->ResultCode=1;
  }
 }
 return 0;
}
static NTSTATUS TelemetryEscapeFlags(void *data, unsigned size, int hardware)
{
    BC250_ESCAPE *head = data;
    BC250_ESCAPE_DPM *d = data;
    escapeHardware = hardware;
    escapeCalls++; escapeSize = size;
    if (head->Command == BC250_ESCAPE_RUN_DPM_CURVE) return TelemetryCurve(data);
    if (head->Command == BC250_ESCAPE_RUN_CPU) return TelemetryCpu(data);
    if (head->Command == BC250_ESCAPE_RUN_FAN) return TelemetryFan(data);
    if (head->Command == BC250_ESCAPE_RUN_BOARD_MEMORY) return TelemetryUma(data);
    sent = *d;
    if (escapeMode == 1) return (NTSTATUS)0xC00000A3;          // a KMD before the DPM escape
    if (escapeMode == 2) return 0;                             // untouched: UNKNOWN_COMMAND
    d->Status = BC250_ESCAPE_STATUS_DONE; d->Version = 0x000700B1u;
    d->Flags = BC250_DPM_FLAG_TEMPERATURE | BC250_DPM_FLAG_CLOCK | BC250_DPM_FLAG_HW_BUSY;
    d->TemperatureMc = 67500; d->ObservedMHz = 1000; d->BusyPermille = 910; d->BusyAvgPermille = 880;
    if (size == BC250_DPM_ABI3_SIZE) {                        // ABI 3 (0.7.215): the SMU metrics tail
        BC250_ESCAPE_DPM_EX *x = data;
        x->Metrics.MetricsState = BC250_DPM_METRICS_OK; x->Metrics.MetricsReads = 12;
        x->Metrics.SocketPowerMw = 78000; x->Metrics.SocketPowerAvgMw = 77400;
        d->Flags |= BC250_DPM_FLAG_POWER;
    }
    if (escapeMode == 3) { d->Status = BC250_ESCAPE_STATUS_REFUSED; d->NtStatus = 0xC000000Du; }
    if (escapeMode == 4) { d->Status = BC250_ESCAPE_STATUS_REFUSED; d->NtStatus = 0; }
    if (escapeMode == 5) d->AbiVersion = 2;
    if (escapeMode == 6) d->Command = 21;
    return 0;
}
static NTSTATUS TelemetryEscape(void *data, unsigned size)
{
    return TelemetryEscapeFlags(data, size, 0);
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
    /* ABI 3 (0.7.215): a caller with a BC250_ESCAPE_DPM_EX passes 248 bytes and gets the metrics tail. */
    {
        BC250_ESCAPE_DPM_EX x;
        CHECK(sizeof(x) == BC250_DPM_ABI3_SIZE && sizeof(x.Dpm) == BC250_DPM_ABI2_SIZE && sizeof(x.Metrics) == 56);
        escapeCalls = 0;
        CHECK(Bc250Dpm(&x.Dpm, 247) < 0 && Bc250Dpm(&x.Dpm, 249) < 0 && escapeCalls == 0);
        memset(&x, 0xA5, sizeof(x));
        CHECK(Bc250Dpm(&x.Dpm, BC250_DPM_ABI3_SIZE) == 0 && escapeCalls == 1 && escapeSize == BC250_DPM_ABI3_SIZE);
        CHECK(sent.AbiVersion == BC250_DPM_ABI_3 && sent.Command == 23 && sent.Op == 0 && sent.IdleMHz == 0);
        CHECK((x.Dpm.Flags & BC250_DPM_FLAG_POWER) && x.Metrics.MetricsState == BC250_DPM_METRICS_OK);
        CHECK(x.Metrics.SocketPowerMw == 78000 && x.Metrics.SocketPowerAvgMw == 77400 && x.Metrics.GfxMv == 0);
        /* An answer that carries another AbiVersion is refused, as for the other two sizes. */
        escapeMode = 5; CHECK(Bc250Dpm(&x.Dpm, BC250_DPM_ABI3_SIZE) == (LONG)0xC000000D);
        escapeMode = 0;
    }

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
    /* ---- the V/F curve and the CPU surface (0.7.210) ------------------------------------------------------ */
    {
        BC250_ESCAPE_DPM_CURVE c;
        BC250_ESCAPE_CPU u;
        BC250_CPU_REQUEST r;
        ULONG mv[BC250_DPM_CURVE_POINTS];
        unsigned i;
        for (i = 0; i < BC250_DPM_CURVE_POINTS; i++) mv[i] = 820 + i * 12;
        CHECK(sizeof(c) == 360 && sizeof(u) == 296 && sizeof(r) == 56 && BC250_DPM_CURVE_POINTS == 11);
        escapeCalls = 0; escapeHardware = -1;
        /* Refused in the DLL, before any escape: no buffer, a wrong size, an operation that does not exist, and a
         * SET without the 11 values. */
        CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, NULL, sizeof(c)) == (LONG)0xC000000D);
        CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, 359) == (LONG)0xC000000D);
        CHECK(Bc250DpmCurve(9, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC000000D);
        CHECK(Bc250DpmCurve(BC250_DPM_CURVE_OP_SET, 7, NULL, BC250_DPM_CURVE_POINTS, 0, &c, sizeof(c)) == (LONG)0xC000000D);
        CHECK(Bc250DpmCurve(BC250_DPM_CURVE_OP_SET, 7, mv, 10, 0, &c, sizeof(c)) == (LONG)0xC000000D);
        CHECK(escapeCalls == 0 && escapeHardware == -1);
        /* A READ: software state, no generation, and the reply's own fields. */
        CHECK(Bc250DpmCurve(BC250_DPM_CURVE_OP_READ, 99, NULL, 0, 0, &c, sizeof(c)) == 0);
        CHECK(escapeCalls == 1 && escapeHardware == 0 && escapeSize == sizeof(c));
        CHECK(sentCurve.Magic == BC250_ESCAPE_MAGIC && sentCurve.Command == BC250_ESCAPE_RUN_DPM_CURVE);
        CHECK(sentCurve.AbiVersion == BC250_DPM_CURVE_ABI && sentCurve.Op == BC250_DPM_CURVE_OP_READ);
        CHECK(sentCurve.ExpectedGeneration == 0 && sentCurve.TrialMs == 0 && sentCurve.CandidateMv[0] == 0);
        CHECK(sentCurve.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND && !sentCurve.Reserved[0] && !sentCurve.Reserved[1]);
        CHECK(c.Points == BC250_DPM_CURVE_POINTS && c.ActiveMv[0] == 820 && c.ActiveMv[10] == 920 && c.Serial == 3);
        /* A SET: the candidate and the window travel, and the generation of the read it is built on. */
        CHECK(Bc250DpmCurve(BC250_DPM_CURVE_OP_SET, 4242, mv, BC250_DPM_CURVE_POINTS, 30000, &c, sizeof(c)) == 0);
        CHECK(escapeCalls == 2 && escapeHardware == 0 && sentCurve.Op == BC250_DPM_CURVE_OP_SET);
        CHECK(sentCurve.ExpectedGeneration == 4242 && sentCurve.TrialMs == 30000);
        CHECK(sentCurve.CandidateMv[0] == 820 && sentCurve.CandidateMv[10] == 820 + 120);
        /* KEEP, CANCEL and RESET carry the generation and no candidate. */
        CHECK(Bc250DpmCurve(BC250_DPM_CURVE_OP_KEEP, 4242, mv, BC250_DPM_CURVE_POINTS, 30000, &c, sizeof(c)) == 0);
        CHECK(sentCurve.Op == BC250_DPM_CURVE_OP_KEEP && sentCurve.ExpectedGeneration == 4242);
        CHECK(sentCurve.CandidateMv[0] == 0 && sentCurve.TrialMs == 0);
        curveMode = 1; CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC00000A3);
        curveMode = 2; CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC00000BB);
        curveMode = 3; CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC000000D);
        curveMode = 4; CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC00000A3);
        curveMode = 5; CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC000000D);
        curveMode = 6; CHECK(Bc250DpmCurve(0, 0, NULL, 0, 0, &c, sizeof(c)) == (LONG)0xC000000D);
        curveMode = 0;

        memset(&r, 0, sizeof(r));
        escapeCalls = 0; escapeHardware = -1;
        /* The request structure names its own size, so an older caller and a newer DLL refuse each other. */
        CHECK(Bc250Cpu(NULL, &u, sizeof(u)) == (LONG)0xC000000D);
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC000000D);     /* Size 0 */
        r.Size = sizeof(r);
        CHECK(Bc250Cpu(&r, NULL, sizeof(u)) == (LONG)0xC000000D);
        CHECK(Bc250Cpu(&r, &u, 295) == (LONG)0xC000000D);
        r.Op = 9; CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC000000D);
        CHECK(escapeCalls == 0 && escapeHardware == -1);
        /* READ is the software snapshot: no HardwareAccess, no generation. */
        r.Op = BC250_CPU_OP_READ; r.ExpectedGeneration = 77;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeCalls == 1 && escapeHardware == 0);
        CHECK(sentCpu.Command == BC250_ESCAPE_RUN_CPU && sentCpu.AbiVersion == BC250_CPU_ABI);
        CHECK(sentCpu.Op == BC250_CPU_OP_READ && sentCpu.ExpectedGeneration == 0 && sentCpu.Given == 0);
        CHECK(u.VoltageMv == 1050 && u.Cores == 6 && u.CoreMask == 0x77);
        /* READBACK and every write go with HardwareAccess and the generation. */
        r.Op = BC250_CPU_OP_READBACK;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 1 && sentCpu.ExpectedGeneration == 77);
        r.Op = BC250_CPU_OP_SET; r.Given = BC250_CPU_GIVEN_MAX | BC250_CPU_GIVEN_UV;
        r.MaxMHz = 3300; r.UvSteps = 6; r.TempC = 95; r.TrialMs = 40000;
        r.WheaEvents = 2; r.ChecksumErrors = 3; r.Loaded = 1;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 1);
        CHECK(sentCpu.Given == (BC250_CPU_GIVEN_MAX | BC250_CPU_GIVEN_UV) && sentCpu.MaxMHz == 3300);
        CHECK(sentCpu.UvSteps == 6 && sentCpu.TempC == 95 && sentCpu.TrialMs == 40000);
        /* The sample the caller alone can see travels on SET and on SEARCH_STEP (0.7.211). */
        CHECK(sentCpu.WheaEvents == 2 && sentCpu.ChecksumErrors == 3 && sentCpu.Loaded == 1);
        r.Op = BC250_CPU_OP_SEARCH_STEP;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 1 && sentCpu.Given == 0);
        CHECK(sentCpu.WheaEvents == 2 && sentCpu.ChecksumErrors == 3 && sentCpu.Loaded == 1);
        /* KEEP sends none of the three values: it keeps what the driver already applied, and judges nothing. It
           reaches no mailbox either, so it is the one write that goes with the software flag word (0.7.213). */
        r.Op = BC250_CPU_OP_KEEP;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 0 &&
              sentCpu.Given == 0 && sentCpu.MaxMHz == 0 && sentCpu.TrialMs == 0);
        CHECK(sentCpu.WheaEvents == 0 && sentCpu.ChecksumErrors == 0 && sentCpu.Loaded == 0);
        r.Op = BC250_CPU_OP_SEARCH_BEGIN;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && sentCpu.UvSteps == 6 && sentCpu.TrialMs == 40000 && sentCpu.Given == 0);
        CHECK(sentCpu.WheaEvents == 0 && sentCpu.Loaded == 0);
        r.Op = BC250_CPU_OP_CORES; r.CoreMask = 255;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && sentCpu.CoreMask == 255 && sentCpu.UvSteps == 0);
        r.Op = BC250_CPU_OP_READ;
        cpuMode = 1; CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC00000A3);
        cpuMode = 2; CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC00000BB);
        cpuMode = 3; CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC0000061);
        cpuMode = 4; CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC00000A3);
        cpuMode = 5; CHECK(Bc250Cpu(&r, &u, sizeof(u)) == (LONG)0xC000000D);
        /* Against a driver of 0.7.212 or older the refused KEEP goes again with the old flag word, with the request
           rebuilt, and every later KEEP of this process goes that way at once. READ keeps the software word. */
        cpuMode = 6; r.Op = BC250_CPU_OP_KEEP; r.ExpectedGeneration = 77; escapeCalls = 0;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 1 && escapeCalls == 2 &&
              sentCpu.Op == BC250_CPU_OP_KEEP && sentCpu.ExpectedGeneration == 77);
        escapeCalls = 0;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 1 && escapeCalls == 1);
        cpuMode = 0; r.Op = BC250_CPU_OP_READ; escapeCalls = 0;
        CHECK(Bc250Cpu(&r, &u, sizeof(u)) == 0 && escapeHardware == 0 && escapeCalls == 1);
    }
    {
        /* The fan control (0.7.2xx, RUN_FAN): every operation is a software request, a write carries the generation
           and its fields, a READ carries none of them, and a malformed request sends nothing. */
        BC250_FAN_REQUEST r;
        BC250_ESCAPE_FAN f;
        memset(&r, 0, sizeof(r));
        escapeCalls = 0; escapeHardware = -1;
        CHECK(Bc250Fan(NULL, &f, sizeof(f)) == (LONG)0xC000000D);
        CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D);     /* Size 0 */
        r.Size = sizeof(r);
        CHECK(Bc250Fan(&r, NULL, sizeof(f)) == (LONG)0xC000000D);
        CHECK(Bc250Fan(&r, &f, 271) == (LONG)0xC000000D);
        r.Op = 5; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D);
        r.Op = BC250_FAN_OP_CURVE; r.Points = 9; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D);
        r.Points = 0; r.Reserved = 1; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D);
        r.Reserved = 0;
        CHECK(escapeCalls == 0 && escapeHardware == -1);
        r.Op = BC250_FAN_OP_READ; r.ExpectedGeneration = 77; r.FixedPct = 40; r.Store = 1;
        CHECK(Bc250Fan(&r, &f, sizeof(f)) == 0 && escapeCalls == 1 && escapeHardware == 0);
        CHECK(sentFan.Command == BC250_ESCAPE_RUN_FAN && sentFan.AbiVersion == BC250_FAN_ABI);
        CHECK(sentFan.Op == BC250_FAN_OP_READ && sentFan.ExpectedGeneration == 0 && sentFan.FixedPct == 0 &&
              sentFan.Store == 0);
        CHECK(f.State == 2 && f.Rpm == 1180 && f.Generation == 77);
        r.Op = BC250_FAN_OP_FIXED; r.FixedPct = 40; r.LeaseMs = 30000; r.Store = 0;
        CHECK(Bc250Fan(&r, &f, sizeof(f)) == 0 && escapeHardware == 0);
        CHECK(sentFan.Op == BC250_FAN_OP_FIXED && sentFan.FixedPct == 40 && sentFan.LeaseMs == 30000 &&
              sentFan.ExpectedGeneration == 77);
        r.Op = BC250_FAN_OP_CURVE; r.Profile = 0; r.Points = 2; r.CurveC[0] = 40; r.CurvePct[0] = 30;
        r.CurveC[1] = 80; r.CurvePct[1] = 100; r.CurveC[2] = 90; r.CurvePct[2] = 100; r.LeaseMs = 0; r.Store = 1;
        CHECK(Bc250Fan(&r, &f, sizeof(f)) == 0 && escapeHardware == 0);
        CHECK(sentFan.Points == 2 && sentFan.CurveC[1] == 80 && sentFan.CurvePct[1] == 100 && sentFan.Store == 1);
        CHECK(sentFan.CurveC[2] == 0 && sentFan.CurvePct[2] == 0);    /* nothing past Points travels */
        r.Op = BC250_FAN_OP_READ;
        fanMode = 1; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC00000A3);
        fanMode = 2; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC00000BB);
        fanMode = 3; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC0000022 && f.Error == 0);
        fanMode = 4; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC00000A3);
        fanMode = 5; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D);
        fanMode = 6; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D);
        /* A refused write names the rule in Error even though the call fails. */
        r.Op = BC250_FAN_OP_FIXED; r.FixedPct = 10; r.LeaseMs = 30000;
        fanMode = 7; CHECK(Bc250Fan(&r, &f, sizeof(f)) == (LONG)0xC000000D && f.Error == 5);
        fanMode = 0;
    }
    escapeCalls = 0;
    statsMode = 1; CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == (LONG)0xC0000001);
    statsMode = 2; CHECK(Bc250VideoMemory(NULL, &m, sizeof(m)) == (LONG)0xC0000001);
    statsMode = 0;
    {
        BC250_CONTROL_BOARD_MEMORY_REQUEST r; BC250_ESCAPE_BOARD_MEMORY u; unsigned i;
        memset(&r,0,sizeof(r));r.Size=sizeof(r);umaMode=0;umaHardwareCalls=0;escapeCalls=0;
        CHECK(sizeof(r)==48 && offsetof(BC250_CONTROL_BOARD_MEMORY_REQUEST,ExpectedBlock)==16);
        CHECK(offsetof(BC250_CONTROL_BOARD_MEMORY_REQUEST,ReservedEnd)==44);
        CHECK(sizeof(u)==128 && offsetof(BC250_ESCAPE_BOARD_MEMORY,ProviderId)==80 &&
              offsetof(BC250_ESCAPE_BOARD_MEMORY,AllowedMiB)==84 &&
              offsetof(BC250_ESCAPE_BOARD_MEMORY,NeedsRestart)==92 &&
              offsetof(BC250_ESCAPE_BOARD_MEMORY,Reserved)==96);
        CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==0 && escapeCalls==1 && escapeHardware==0);
        CHECK(u.ActiveBytes==(12ull<<30) && u.Flags==(BC250_BOARD_MEMORY_ACTIVE_VALID|BC250_BOARD_MEMORY_SUPPORTED));
        CHECK(sentUma.Status==BC250_ESCAPE_STATUS_UNKNOWN_COMMAND && sentUma.RequestedMiB==0);
        CHECK(Bc250BoardMemory(NULL,sizeof(u),&r)==(LONG)0xC000000D);
        CHECK(Bc250BoardMemory(&u,sizeof(u),NULL)==(LONG)0xC000000D);
        CHECK(Bc250BoardMemory(&u,127,&r)==(LONG)0xC000000D);
        r.Size--;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);r.Size++;
        r.Reserved=1;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);r.Reserved=0;
        r.ReservedEnd=1;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);r.ReservedEnd=0;
        r.Op=3;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);r.Op=0;
        r.ExpectedBlock[27]=1;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);r.ExpectedBlock[27]=0;
        CHECK(escapeCalls==1);
        umaMode=1;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC00000BB);
        umaMode=2;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC00000A3);
        for(i=3;i<=8;i++){umaMode=(int)i;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);}
        umaMode=9;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC0000022);
        for(i=12;i<=13;i++){umaMode=(int)i;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);}
        for(i=15;i<=21;i++){umaMode=(int)i;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D);}
        umaMode=14;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==0 && u.Flags==0 && u.ProviderId==0);
        r.Op=BC250_BOARD_MEMORY_OP_SET;r.TargetMiB=8192;umaHardwareCalls=0;escapeCalls=0;
        CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC00000BB && escapeCalls==1 && umaHardwareCalls==0);
        umaMode=0;r.Op=BC250_BOARD_MEMORY_OP_SET;r.TargetMiB=8192;escapeCalls=0;
        CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC00000BB && escapeCalls==1 && umaHardwareCalls==0);
        r.TargetMiB=14336;escapeCalls=0;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D && escapeCalls==0);
        r.TargetMiB=12288;umaMode=10;escapeCalls=0;
        CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000022D && escapeCalls==1 && umaHardwareCalls==0);
        UmaMockBlock(r.ExpectedBlock,8192);escapeCalls=0;
        CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==0 && escapeCalls==2 && umaHardwareCalls==1 && escapeHardware==1);
        CHECK(sentUma.RequestedMiB==12288 && !memcmp(sentUma.ObservedBlock,r.ExpectedBlock,28));
        r.Op=BC250_BOARD_MEMORY_OP_RESTORE;r.TargetMiB=0;umaHardwareCalls=0;
        CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC00000BB && umaHardwareCalls==0);
        umaMode=11;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==0 && umaHardwareCalls==1);
        r.Op=BC250_BOARD_MEMORY_OP_SET;r.TargetMiB=12288;
        for(i=22;i<=25;i++){umaMode=(int)i;umaHardwareCalls=0;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC000000D && umaHardwareCalls==1);}
        umaMode=26;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==(LONG)0xC00000A3 && u.Reason==6);
        umaMode=27;CHECK(Bc250BoardMemory(&u,sizeof(u),&r)==0 && u.ResultCode==1 && u.RequestedMiB==12288);
        umaMode=0;
    }
    {
        BC250_ESCAPE_BOARD_MEMORY_PROBE p;
        umaMode=0;escapeCalls=0;
        CHECK(Bc250BoardMemoryProbe(NULL,sizeof(p))==(LONG)0xC000000D);
        CHECK(Bc250BoardMemoryProbe(&p,127)==(LONG)0xC000000D && escapeCalls==0);
        CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==0 && escapeCalls==1 && escapeHardware==1);
        CHECK(p.SlotCount==28 && p.OffsetCount==0 && p.SlotBlock[0]==0x24 && p.RtcValue[3]==0x80);
        umaMode=1;CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==(LONG)0xC00000BB);
        umaMode=2;CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==(LONG)0xC0000022);
        umaMode=3;CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==(LONG)0xC000000D);
        umaMode=4;CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==(LONG)0xC000000D);
        umaMode=5;CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==(LONG)0xC00000BB);
        umaMode=6;CHECK(Bc250BoardMemoryProbe(&p,sizeof(p))==(LONG)0xC000000D);
    }
    printf("Telemetry exports: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
