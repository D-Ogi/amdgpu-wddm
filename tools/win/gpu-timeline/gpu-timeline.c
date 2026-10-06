// gpu-timeline.exe - GPU block activity and command-processor state over time, sampled through bc250rd.sys
// (H12 instrument: how much of the GPU's "busy" time is the CP held in waits rather than shader work).
//
//   gpu-timeline probe    [--set full|lite] [--reads N]
//   gpu-timeline sample   --seconds S --out FILE [--hz H] [--set full|lite] [--stop-file PATH] [--cpu N]
//                         [--priority normal|high]
//   gpu-timeline selftest --out FILE
//
// probe     attaches, reads GRBM_GFX_INDEX and every register of the set once (printed in hex), then times N batched
//           reads back to back (default 200, at most 2000) and prints the cost of one batch. Under a second.
// sample    one batched read every 1/H s (default 1009 Hz, 100..5000) for S s (1..60, hard cap), all in memory; the
//           file (format below) is written after the run, then a one-screen summary goes to stdout. Ends early when
//           the stop file appears (checked about every 100 ms) or after three failed reads in a row.
// selftest  writes a synthetic file with a known composition (no device): the analyzer's test input.
//
// Why bc250rd and not bc250kmd: the KMD's READ_REG escape runs only as a HardwareAccess (Level Two) escape
// (driver/kmd/display.c refuses it with NoAdapterSynchronization), and Level Two guarantees an idle GPU with no DMA
// buffer in flight: every sample would wait for idle and could never see the GPU busy. bc250rd maps BAR5 read-only
// on its own, outside dxgkrnl's synchronization, and reads allow-listed offsets (facts M30: both drivers read the
// same values).
//
// What it does to the GPU: IOCTL_BC250RD_ATTACH and IOCTL_BC250RD_READ, nothing else - never SMU_MSG, never SMN.
// The offsets come from gen_regs.py (tools/regcalc, checked against bc250rd's allow-list and facts M25).
// What it does to the machine: one thread woken by a high-resolution waitable timer and one IOCTL per sample. No
// timeBeginPeriod (the game's timer resolution stays as it is), no file I/O until the run is over.
//
// File format (little endian): GTL_HEADER, NRegs x GTL_REGDESC, NSamples x record. A record is
//   u64 qpc (QueryPerformanceCounter before the IOCTL), u32 read ticks (QPC ticks the IOCTL took),
//   u32 status (0, or the Win32 error of a failed read; values are 0 then), u32 value[NRegs] in read order.
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc250rd_ioctl.h"
#include "gc_10_1_0_sh_mask.h"
#include "gtl_regs.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define GTL_VERSION 1u
#define GTL_MAX_SECONDS 60u
#define GTL_MIN_HZ 100u
#define GTL_MAX_HZ 5000u
#define GTL_DEFAULT_HZ 1009u        // prime: no phase lock with 1 ms periodic work (the KMD's DPM sampler is 1 kHz)
#define GTL_MAX_REGS 32u
#define GTL_FAIL_LIMIT 3u

#pragma pack(push, 1)
typedef struct _GTL_HEADER {
    char Magic[4];                  // "GTL1"
    UINT32 Version, HeaderBytes, RegDescBytes, RecordBytes, NRegs;
    UINT32 Hz, SecondsRequested, NSamples, Late, Failures, SlowReads;
    UINT64 QpcFrequency, QpcStart, QpcEnd;
    UINT64 FileTimeStart, FileTimeEnd;      // UTC, GetSystemTimePreciseAsFileTime, next to QpcStart/QpcEnd
    UINT32 GfxIndexStart, GfxIndexEnd;      // GRBM_GFX_INDEX before and after
    UINT32 Flags;                           // GTL_FLAG_*
    UINT32 ProcessId;
    char Set[8];                            // "full", "lite", "synth" (selftest)
    UINT32 MaxReadTicks, Reserved;
} GTL_HEADER;
typedef struct _GTL_REGDESC {
    UINT32 Offset;
    char Name[28];
} GTL_REGDESC;
#pragma pack(pop)

#define GTL_FLAG_STOPPED_BY_FILE 1u
#define GTL_FLAG_STOPPED_BY_FAILURES 2u
#define GTL_FLAG_SYNTHETIC 4u
#define GTL_FLAG_SAMPLE_CAP 8u

typedef struct _GTL_RUN {
    const GTL_REG* Regs;
    UINT32 NRegs;
    const char* SetName;
    UINT32 RecordBytes;
    BYTE* Records;
    UINT32 NSamples, Capacity;
    GTL_HEADER Header;
} GTL_RUN;

static UINT64 Qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (UINT64)t.QuadPart;
}

static UINT64 QpcFrequency(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (UINT64)f.QuadPart;
}

static UINT64 FileTimeNow(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static int SelectSet(const char* Name, GTL_RUN* Run)
{
    if (strcmp(Name, "full") == 0) { Run->Regs = g_GtlFull; Run->NRegs = GTL_FULL_COUNT; }
    else if (strcmp(Name, "lite") == 0) { Run->Regs = g_GtlLite; Run->NRegs = GTL_LITE_COUNT; }
    else { fprintf(stderr, "unknown set %s (full or lite)\n", Name); return 0; }
    Run->SetName = Name;
    Run->RecordBytes = 16u + 4u * Run->NRegs;
    return Run->NRegs <= GTL_MAX_REGS;
}

static HANDLE OpenReader(void)
{
    HANDLE h = CreateFileA(BC250RD_DEVICE_USER, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        fprintf(stderr, "cannot open %s (error %lu): is bc250rd.sys loaded, is this an elevated prompt?\n",
                BC250RD_DEVICE_USER, GetLastError());
    return h;
}

static int Attach(HANDLE h, BC250RD_INFO* Info)
{
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_BC250RD_ATTACH, NULL, 0, Info, sizeof(*Info), &got, NULL)) {
        fprintf(stderr, "attach failed, error %lu\n", GetLastError());
        return 0;
    }
    return 1;
}

// One batch: the driver checks every offset against its allow-list before it reads any (all or nothing).
static DWORD ReadBatch(HANDLE h, const ULONG* Offsets, UINT32 Count, ULONG* Values)
{
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_BC250RD_READ, (LPVOID)Offsets, Count * sizeof(ULONG), Values, Count * sizeof(ULONG),
                         &got, NULL))
        return GetLastError();
    return got == Count * sizeof(ULONG) ? 0 : ERROR_INVALID_DATA;
}

static DWORD ReadOne(HANDLE h, ULONG Offset, ULONG* Value)
{
    return ReadBatch(h, &Offset, 1, Value);
}

static void FillHeader(GTL_RUN* Run, UINT32 Hz, UINT32 Seconds)
{
    GTL_HEADER* hd = &Run->Header;
    memcpy(hd->Magic, "GTL1", 4);
    hd->Version = GTL_VERSION;
    hd->HeaderBytes = sizeof(GTL_HEADER);
    hd->RegDescBytes = sizeof(GTL_REGDESC);
    hd->RecordBytes = Run->RecordBytes;
    hd->NRegs = Run->NRegs;
    hd->Hz = Hz;
    hd->SecondsRequested = Seconds;
    hd->QpcFrequency = QpcFrequency();
    hd->ProcessId = GetCurrentProcessId();
    strncpy(hd->Set, Run->SetName, sizeof(hd->Set) - 1);
}

static int WriteRun(const char* Path, GTL_RUN* Run)
{
    FILE* f = fopen(Path, "wb");
    UINT32 i;
    if (f == NULL) { fprintf(stderr, "cannot write %s\n", Path); return 0; }
    Run->Header.NSamples = Run->NSamples;
    fwrite(&Run->Header, sizeof(Run->Header), 1, f);
    for (i = 0; i < Run->NRegs; i++) {
        GTL_REGDESC d;
        memset(&d, 0, sizeof(d));
        d.Offset = Run->Regs[i].Offset;
        strncpy(d.Name, Run->Regs[i].Name, sizeof(d.Name) - 1);
        fwrite(&d, sizeof(d), 1, f);
    }
    fwrite(Run->Records, Run->RecordBytes, Run->NSamples, f);
    if (fclose(f) != 0) { fprintf(stderr, "write error on %s\n", Path); return 0; }
    return 1;
}

static int CompareU32(const void* A, const void* B)
{
    UINT32 a = *(const UINT32*)A, b = *(const UINT32*)B;
    return a < b ? -1 : a > b;
}

static int FindReg(const GTL_RUN* Run, const char* Name)
{
    UINT32 i;
    for (i = 0; i < Run->NRegs; i++) if (strcmp(Run->Regs[i].Name, Name) == 0) return (int)i;
    return -1;
}

// The quick look: the analyzer (analyze.py) does the real split. Masks from the vendored gc_10_1_0_sh_mask.h.
static void PrintSummary(const GTL_RUN* Run)
{
    const GTL_HEADER* hd = &Run->Header;
    const UINT32 pipe = GRBM_STATUS__SPI_BUSY_MASK | GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__GDS_BUSY_MASK |
                        GRBM_STATUS__GE_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK | GRBM_STATUS__BCI_BUSY_MASK |
                        GRBM_STATUS__SC_BUSY_MASK | GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__DB_BUSY_MASK |
                        GRBM_STATUS__CB_BUSY_MASK;
    int grbm = FindReg(Run, "GRBM_STATUS"), sdma = FindReg(Run, "SDMA0_STATUS_REG");
    UINT32 i, ok = 0, active = 0, piped = 0, cponly = 0, sdmaBusy = 0;
    UINT32* ticks = (UINT32*)malloc(sizeof(UINT32) * (Run->NSamples ? Run->NSamples : 1));
    double us = 1e6 / (double)hd->QpcFrequency, span, sum = 0;

    for (i = 0; i < Run->NSamples; i++) {
        const BYTE* r = Run->Records + (SIZE_T)i * Run->RecordBytes;
        const UINT32* v = (const UINT32*)(r + 16);
        UINT32 t = *(const UINT32*)(r + 8), status = *(const UINT32*)(r + 12);
        if (ticks) ticks[i] = t;
        sum += t;
        if (status != 0) continue;
        ok++;
        if (v[grbm] & GRBM_STATUS__GUI_ACTIVE_MASK) {
            active++;
            if (v[grbm] & pipe) piped++; else cponly++;
        }
        if (sdma >= 0 && (v[sdma] & SDMA0_STATUS_REG__IDLE_MASK) == 0) sdmaBusy++;
    }
    span = (double)(hd->QpcEnd - hd->QpcStart) / (double)hd->QpcFrequency;
    printf("set %s, %u samples in %.3f s = %.1f/s (asked %u/s), %u late periods skipped, %u failed reads, flags 0x%X\n",
           Run->SetName, Run->NSamples, span, span > 0 ? Run->NSamples / span : 0.0, hd->Hz, hd->Late, hd->Failures,
           hd->Flags);
    if (ticks && Run->NSamples) {
        qsort(ticks, Run->NSamples, sizeof(UINT32), CompareU32);
        printf("read of %u registers: mean %.1f us, p50 %.1f, p99 %.1f, max %.1f; reads over 1 ms: %u\n", Run->NRegs,
               sum / Run->NSamples * us, ticks[Run->NSamples / 2] * us, ticks[(UINT32)(Run->NSamples * 0.99)] * us,
               ticks[Run->NSamples - 1] * us, hd->SlowReads);
    }
    free(ticks);
    printf("GRBM_GFX_INDEX 0x%08X before, 0x%08X after\n", hd->GfxIndexStart, hd->GfxIndexEnd);
    if (ok) {
        printf("GUI_ACTIVE %.1f %%: pipeline (SPI/TA/GDS/GE/SX/BCI/SC/PA/DB/CB) %.1f %%, CP only %.1f %%; idle %.1f %%",
               100.0 * active / ok, 100.0 * piped / ok, 100.0 * cponly / ok, 100.0 * (ok - active) / ok);
        if (sdma >= 0) printf("; SDMA0 busy %.1f %%", 100.0 * sdmaBusy / ok);
        printf("\n");
    }
}

static UINT32 ParseU32(const char* S, UINT32 Min, UINT32 Max, const char* What, int* Ok)
{
    char* end = NULL;
    unsigned long v = strtoul(S, &end, 0);
    if (end == S || *end != 0 || v < Min || v > Max) {
        fprintf(stderr, "%s must be %u..%u, got %s\n", What, Min, Max, S);
        *Ok = 0;
    }
    return (UINT32)v;
}

static int Probe(const char* SetName, UINT32 Reads)
{
    GTL_RUN run;
    BC250RD_INFO info;
    ULONG offsets[GTL_MAX_REGS], values[GTL_MAX_REGS], gfxIndex = 0;
    UINT32* ticks;
    UINT32 i;
    double us = 1e6 / (double)QpcFrequency(), sum = 0;
    HANDLE h;
    DWORD err;

    memset(&run, 0, sizeof(run));
    if (!SelectSet(SetName, &run)) return 2;
    h = OpenReader();
    if (h == INVALID_HANDLE_VALUE) return 1;
    memset(&info, 0, sizeof(info));
    if (!Attach(h, &info)) { CloseHandle(h); return 1; }
    printf("bc250rd: %04X:%04X at %lu:%lu.%lu, BAR5 size 0x%lX, allow-list %lu offsets\n", info.VendorId,
           info.DeviceId, info.Bus, info.Device, info.Function, info.Bar5Size, info.AllowCount);
    err = ReadOne(h, GTL_REG_GRBM_GFX_INDEX, &gfxIndex);
    printf("GRBM_GFX_INDEX 0x%05lX = 0x%08lX (status %lu)\n", (ULONG)GTL_REG_GRBM_GFX_INDEX, gfxIndex, err);
    for (i = 0; i < run.NRegs; i++) offsets[i] = run.Regs[i].Offset;
    err = ReadBatch(h, offsets, run.NRegs, values);
    if (err != 0) {
        fprintf(stderr, "batch read failed, error %lu (an offset outside the deployed allow-list fails the batch)\n", err);
        CloseHandle(h);
        return 1;
    }
    for (i = 0; i < run.NRegs; i++) printf("%-22s 0x%05lX = 0x%08lX\n", run.Regs[i].Name, offsets[i], values[i]);
    ticks = (UINT32*)malloc(sizeof(UINT32) * Reads);
    if (ticks == NULL) { CloseHandle(h); return 1; }
    for (i = 0; i < Reads; i++) {
        UINT64 t0 = Qpc();
        err = ReadBatch(h, offsets, run.NRegs, values);
        ticks[i] = (UINT32)(Qpc() - t0);
        sum += ticks[i];
        if (err != 0) { fprintf(stderr, "read %u failed, error %lu\n", i, err); free(ticks); CloseHandle(h); return 1; }
    }
    qsort(ticks, Reads, sizeof(UINT32), CompareU32);
    printf("%u batches of %u registers back to back: mean %.1f us, p50 %.1f, p99 %.1f, max %.1f (%.2f us a register)\n",
           Reads, run.NRegs, sum / Reads * us, ticks[Reads / 2] * us, ticks[(UINT32)(Reads * 0.99)] * us,
           ticks[Reads - 1] * us, sum / Reads * us / run.NRegs);
    free(ticks);
    CloseHandle(h);
    return 0;
}

static int Sample(const char* SetName, UINT32 Seconds, UINT32 Hz, const char* Out, const char* StopFile, int Cpu,
                  int HighPriority)
{
    GTL_RUN run;
    BC250RD_INFO info;
    ULONG offsets[GTL_MAX_REGS];
    HANDLE h, timer;
    UINT64 freq, period, start, end, next, maxTicks = 0;
    UINT32 i, failsInRow = 0, checkEvery;
    WCHAR stopW[MAX_PATH];
    int rc = 0;

    memset(&run, 0, sizeof(run));
    if (!SelectSet(SetName, &run)) return 2;
    stopW[0] = 0;
    if (StopFile && MultiByteToWideChar(CP_UTF8, 0, StopFile, -1, stopW, MAX_PATH) == 0) {
        fprintf(stderr, "bad stop file path\n");
        return 2;
    }
    if (stopW[0] && GetFileAttributesW(stopW) != INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "stop file %s exists before the start: remove it first\n", StopFile);
        return 2;
    }
    run.Capacity = Seconds * Hz + Hz / 10 + 64;
    run.Records = (BYTE*)VirtualAlloc(NULL, (SIZE_T)run.Capacity * run.RecordBytes, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
    if (run.Records == NULL) { fprintf(stderr, "cannot allocate %u records\n", run.Capacity); return 1; }
    // Touch every page now, so that no page fault lands inside the timed loop.
    memset(run.Records, 0, (SIZE_T)run.Capacity * run.RecordBytes);
    FillHeader(&run, Hz, Seconds);
    h = OpenReader();
    if (h == INVALID_HANDLE_VALUE) return 1;
    memset(&info, 0, sizeof(info));
    if (!Attach(h, &info)) { CloseHandle(h); return 1; }
    for (i = 0; i < run.NRegs; i++) offsets[i] = run.Regs[i].Offset;
    {
        ULONG probe[GTL_MAX_REGS];
        DWORD err = ReadBatch(h, offsets, run.NRegs, probe);
        if (err != 0) {
            fprintf(stderr, "first batch read failed, error %lu: nothing sampled\n", err);
            CloseHandle(h);
            return 1;
        }
    }
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == NULL) {
        // A plain waitable timer ticks at the system timer resolution (15.6 ms by default): no 1 kHz without
        // timeBeginPeriod, which would change the game's own timing. Refuse instead.
        fprintf(stderr, "no high-resolution waitable timer (error %lu): refusing\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    if (Cpu >= 0) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << Cpu);
    SetThreadPriority(GetCurrentThread(), HighPriority ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL);
    (void)ReadOne(h, GTL_REG_GRBM_GFX_INDEX, (ULONG*)&run.Header.GfxIndexStart);

    freq = run.Header.QpcFrequency;
    period = freq / Hz;
    checkEvery = Hz / 10 ? Hz / 10 : 1;
    run.Header.FileTimeStart = FileTimeNow();
    start = Qpc();
    run.Header.QpcStart = start;
    end = start + (UINT64)Seconds * freq;
    next = start;
    for (;;) {
        UINT64 now = Qpc(), t0, t1;
        BYTE* r;
        DWORD err;
        if (now >= end) break;
        if (run.NSamples >= run.Capacity) { run.Header.Flags |= GTL_FLAG_SAMPLE_CAP; break; }
        if (stopW[0] && run.NSamples % checkEvery == 0 && GetFileAttributesW(stopW) != INVALID_FILE_ATTRIBUTES) {
            run.Header.Flags |= GTL_FLAG_STOPPED_BY_FILE;
            break;
        }
        if (next > now && (next - now) * 10000000ull / freq >= 500) {   // 50 us or more to go: sleep on the timer
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((next - now) * 10000000ull / freq);
            if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, 100);
        }
        r = run.Records + (SIZE_T)run.NSamples * run.RecordBytes;
        t0 = Qpc();
        err = ReadBatch(h, offsets, run.NRegs, (ULONG*)(r + 16));
        t1 = Qpc();
        *(UINT64*)r = t0;
        *(UINT32*)(r + 8) = (UINT32)min(t1 - t0, 0xFFFFFFFFull);
        *(UINT32*)(r + 12) = err;
        if (t1 - t0 > maxTicks) maxTicks = t1 - t0;
        if ((t1 - t0) * 1000ull > freq) run.Header.SlowReads++;
        run.NSamples++;
        if (err != 0) {
            memset(r + 16, 0, 4u * run.NRegs);
            run.Header.Failures++;
            if (++failsInRow >= GTL_FAIL_LIMIT) { run.Header.Flags |= GTL_FLAG_STOPPED_BY_FAILURES; break; }
        } else failsInRow = 0;
        next += period;
        if (t1 > next + period) {           // fell behind by more than a period: skip ahead, never burst
            run.Header.Late += (UINT32)((t1 - next) / period);
            next = t1 + period;
        }
    }
    run.Header.QpcEnd = Qpc();
    run.Header.FileTimeEnd = FileTimeNow();
    run.Header.MaxReadTicks = (UINT32)min(maxTicks, 0xFFFFFFFFull);
    (void)ReadOne(h, GTL_REG_GRBM_GFX_INDEX, (ULONG*)&run.Header.GfxIndexEnd);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    CloseHandle(timer);
    CloseHandle(h);
    if (!WriteRun(Out, &run)) rc = 1;
    else printf("wrote %s (%u samples, %u bytes a record)\n", Out, run.NSamples, run.RecordBytes);
    PrintSummary(&run);
    VirtualFree(run.Records, 0, MEM_RELEASE);
    return rc;
}

// The analyzer's known input: 10 s at 1 kHz, a 20-sample "frame" of 6 idle, 8 pipeline, 2 pipeline with the ME
// waiting on a partial flush, 2 CP-only surface sync, 1 CP-only memory poll, 1 CP-only packet parsing. Every 50th
// sample's closing GRBM_STATUS differs from its first (a state change inside the read); SDMA0 busy every 10th;
// CP_RB0_RPTR moves every 5th. Expected shares: idle 30 %, pipeline 50 % (drain 10 %), CP-only 20 % (sync 10,
// memory wait 5, parsing 5), bracket changes 2 %, SDMA0 10 %.
static int SelfTest(const char* Out)
{
    GTL_RUN run;
    UINT32 i, n = 10000;
    int g = 0, g2, g3, se0, se1, cs, cb, s1, s2, cpf, sd0, rptr, gEnd;
    const UINT32 pipeBits = GRBM_STATUS__GUI_ACTIVE_MASK | GRBM_STATUS__CP_BUSY_MASK | GRBM_STATUS__SPI_BUSY_MASK |
                            GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK | GRBM_STATUS__CB_BUSY_MASK |
                            GRBM_STATUS__DB_BUSY_MASK | GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__GE_BUSY_MASK;

    memset(&run, 0, sizeof(run));
    SelectSet("full", &run);
    run.SetName = "synth";
    run.Capacity = n;
    run.Records = (BYTE*)calloc(n, run.RecordBytes);
    if (run.Records == NULL) return 1;
    FillHeader(&run, 1000, 10);
    run.Header.QpcFrequency = 10000000ull;
    run.Header.QpcStart = 1000000000ull;
    run.Header.QpcEnd = run.Header.QpcStart + (UINT64)n * 10000ull;
    run.Header.FileTimeStart = 0;
    run.Header.FileTimeEnd = (UINT64)n * 10000ull;
    run.Header.GfxIndexStart = run.Header.GfxIndexEnd = 0xE0000000u;
    run.Header.Flags = GTL_FLAG_SYNTHETIC;
    g2 = FindReg(&run, "GRBM_STATUS2"); g3 = FindReg(&run, "GRBM_STATUS3");
    se0 = FindReg(&run, "GRBM_STATUS_SE0"); se1 = FindReg(&run, "GRBM_STATUS_SE1");
    cs = FindReg(&run, "CP_STAT"); cb = FindReg(&run, "CP_BUSY_STAT");
    s1 = FindReg(&run, "CP_STALLED_STAT1"); s2 = FindReg(&run, "CP_STALLED_STAT2");
    cpf = FindReg(&run, "CP_CPF_STATUS"); sd0 = FindReg(&run, "SDMA0_STATUS_REG"); rptr = FindReg(&run, "CP_RB0_RPTR");
    gEnd = (int)run.NRegs - 1;
    for (i = 0; i < n; i++) {
        BYTE* r = run.Records + (SIZE_T)i * run.RecordBytes;
        UINT32* v = (UINT32*)(r + 16);
        UINT32 k = i % 20;
        *(UINT64*)r = run.Header.QpcStart + (UINT64)i * 10000ull;
        *(UINT32*)(r + 8) = 200;
        v[g] = GRBM_STATUS__DB_CLEAN_MASK | GRBM_STATUS__CB_CLEAN_MASK;
        if (k < 6) {
            // idle
        } else if (k < 14) {
            v[g] |= pipeBits;
            v[se0] = v[se1] = GRBM_STATUS_SE0__SPI_BUSY_MASK | GRBM_STATUS_SE0__PA_BUSY_MASK |
                              GRBM_STATUS_SE0__SC_BUSY_MASK | GRBM_STATUS_SE0__CB_BUSY_MASK;
            v[cs] = CP_STAT__CP_BUSY_MASK | CP_STAT__ME_BUSY_MASK | CP_STAT__PFP_BUSY_MASK;
            v[cb] = CP_BUSY_STAT__ME_PARSING_PACKETS_MASK;
            v[cpf] = CP_CPF_STATUS__CPF_BUSY_MASK;
        } else if (k < 16) {
            v[g] |= GRBM_STATUS__GUI_ACTIVE_MASK | GRBM_STATUS__CP_BUSY_MASK | GRBM_STATUS__SPI_BUSY_MASK;
            v[se0] = GRBM_STATUS_SE0__SPI_BUSY_MASK;
            v[cs] = CP_STAT__CP_BUSY_MASK | CP_STAT__ME_BUSY_MASK;
            v[s2] = CP_STALLED_STAT2__ME_WAITING_ON_PARTIAL_FLUSH_MASK;
        } else if (k < 18) {
            v[g] |= GRBM_STATUS__GUI_ACTIVE_MASK | GRBM_STATUS__CP_BUSY_MASK | GRBM_STATUS__CP_COHERENCY_BUSY_MASK;
            v[cs] = CP_STAT__CP_BUSY_MASK | CP_STAT__SURFACE_SYNC_BUSY_MASK;
            v[g3] = GRBM_STATUS3__GL2CC_BUSY_MASK;
            v[g2] = GRBM_STATUS2__EA_BUSY_MASK;
        } else if (k < 19) {
            v[g] |= GRBM_STATUS__GUI_ACTIVE_MASK | GRBM_STATUS__CP_BUSY_MASK;
            v[cs] = CP_STAT__CP_BUSY_MASK | CP_STAT__ME_BUSY_MASK;
            v[s1] = CP_STALLED_STAT1__ME_WAITING_ON_TC_READ_DATA_MASK;
            v[cb] = CP_BUSY_STAT__SEM_POLLING_FOR_PASS_MASK;
        } else {
            v[g] |= GRBM_STATUS__GUI_ACTIVE_MASK | GRBM_STATUS__CP_BUSY_MASK;
            v[cs] = CP_STAT__CP_BUSY_MASK | CP_STAT__ME_BUSY_MASK | CP_STAT__PFP_BUSY_MASK;
            v[cb] = CP_BUSY_STAT__ME_PARSING_PACKETS_MASK | CP_BUSY_STAT__PFP_PARSING_PACKETS_MASK;
        }
        v[gEnd] = v[g];
        if (i % 50 == 7) v[gEnd] = GRBM_STATUS__DB_CLEAN_MASK | GRBM_STATUS__CB_CLEAN_MASK;   // k = 7: pipeline -> idle
        v[sd0] = (i % 10 == 3) ? 0u : SDMA0_STATUS_REG__IDLE_MASK;
        v[FindReg(&run, "SDMA1_STATUS_REG")] = SDMA0_STATUS_REG__IDLE_MASK;
        v[rptr] = (i / 5) * 16u;
        run.NSamples++;
    }
    if (!WriteRun(Out, &run)) { free(run.Records); return 1; }
    printf("wrote %s (synthetic, %u samples)\n", Out, run.NSamples);
    PrintSummary(&run);
    free(run.Records);
    return 0;
}

static void Usage(void)
{
    fprintf(stderr,
            "gpu-timeline probe    [--set full|lite] [--reads N]\n"
            "gpu-timeline sample   --seconds S --out FILE [--hz H] [--set full|lite] [--stop-file PATH] [--cpu N]\n"
            "                      [--priority normal|high]\n"
            "gpu-timeline selftest --out FILE\n");
}

int main(int argc, char** argv)
{
    const char *cmd, *set = "full", *out = NULL, *stop = NULL;
    UINT32 seconds = 0, hz = GTL_DEFAULT_HZ, reads = 200;
    int cpu = -1, high = 1, ok = 1, i;

    if (argc < 2) { Usage(); return 2; }
    cmd = argv[1];
    for (i = 2; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v == NULL) { fprintf(stderr, "%s needs a value\n", a); return 2; }
        if (strcmp(a, "--set") == 0) set = v;
        else if (strcmp(a, "--out") == 0) out = v;
        else if (strcmp(a, "--stop-file") == 0) stop = v;
        else if (strcmp(a, "--seconds") == 0) seconds = ParseU32(v, 1, GTL_MAX_SECONDS, "--seconds", &ok);
        else if (strcmp(a, "--hz") == 0) hz = ParseU32(v, GTL_MIN_HZ, GTL_MAX_HZ, "--hz", &ok);
        else if (strcmp(a, "--reads") == 0) reads = ParseU32(v, 1, 2000, "--reads", &ok);
        else if (strcmp(a, "--cpu") == 0) cpu = (int)ParseU32(v, 0, 63, "--cpu", &ok);
        else if (strcmp(a, "--priority") == 0) {
            if (strcmp(v, "normal") == 0) high = 0;
            else if (strcmp(v, "high") == 0) high = 1;
            else { fprintf(stderr, "--priority normal|high\n"); ok = 0; }
        } else { fprintf(stderr, "unknown option %s\n", a); ok = 0; }
        i++;
    }
    if (!ok) return 2;
    if (strcmp(cmd, "probe") == 0) return Probe(set, reads);
    if (strcmp(cmd, "sample") == 0) {
        if (seconds == 0 || out == NULL) { Usage(); return 2; }
        return Sample(set, seconds, hz, out, stop, cpu, high);
    }
    if (strcmp(cmd, "selftest") == 0) {
        if (out == NULL) { Usage(); return 2; }
        return SelfTest(out);
    }
    Usage();
    return 2;
}
