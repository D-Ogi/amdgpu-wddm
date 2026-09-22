/* The kernel underneath wddm.c, for the length of one host test run.
 *
 * wddm.c is compiled unmodified, so it still calls ExAllocatePool2, the Ke* spin lock, timer and DPC
 * services, and the driver's own GuardLog / GuardReadSetting / VramFramebufferOffset. This file
 * provides all of them, plus the 22 display and PnP DDIs that WddmBuildTable() stores into the table
 * but this harness never calls.
 *
 * It deliberately includes NO kernel header. Every signature below is written in plain C types that
 * match the kernel ones byte for byte in the x64 ABI (KIRQL and BOOLEAN are UCHAR, LARGE_INTEGER is an
 * 8-byte union passed and returned in a register, PVOID is void*). That is what lets one process hold
 * both halves: qai_bridge.c sees ntifs.h and no CRT, this file sees the CRT and no ntifs.h.
 *
 * The kernel services are declared DECLSPEC_IMPORT in the WDK headers, so wddm.c calls them through an
 * __imp_ slot rather than by name. Defining that slot as a data symbol pointing at our own function is
 * how the call is redirected without touching wddm.c or the headers.
 *
 * Fidelity that matters, and is not decoration:
 *   - ExAllocatePool2 zeroes what it returns. WddmStart() relies on it: it initializes seven members of
 *     BC250_WDDM by hand and leaves the counters, the kind tables and Stopping to the allocator. A stub
 *     handing back malloc() garbage would make this harness pass on a driver that reads uninitialized
 *     state on the lab machine.
 *   - KeGetCurrentIrql answers PASSIVE_LEVEL (0) by default, which is where dxgkrnl calls
 *     DxgkDdiQueryAdapterInfo. bc250h_stub_set_irql() raises it for the paths that check.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qai_bridge.h"

/* ---- the log ring ------------------------------------------------------------------------------ */

#define BC250H_LOG_LINES 512
#define BC250H_LOG_TEXT  256

static char             g_Log[BC250H_LOG_LINES][BC250H_LOG_TEXT];
static unsigned         g_LogCount;
static unsigned long    g_LogSequence;
static int              g_LogEcho;

void bc250h_stub_set_log_echo(int on) { g_LogEcho = on; }

unsigned bc250h_log_count(void) { return g_LogCount; }

const char* bc250h_log_line(unsigned index)
{
    return (index < g_LogCount) ? g_Log[index] : "";
}

void bc250h_log_reset(void) { g_LogCount = 0; }

/* driver\kmd\guard.c */
void GuardLog(const char* Format, ...);
void GuardLog(const char* Format, ...)
{
    va_list args;
    char line[BC250H_LOG_TEXT];

    va_start(args, Format);
    (void)vsnprintf(line, sizeof(line), Format, args);
    va_end(args);
    g_LogSequence++;
    if (g_LogCount < BC250H_LOG_LINES)
    {
        memcpy(g_Log[g_LogCount], line, sizeof(line));
        g_LogCount++;
    }
    if (g_LogEcho) printf("    [kmd] %s\n", line);
}

unsigned long GuardLogSequence(void);
unsigned long GuardLogSequence(void) { return g_LogSequence; }

/* The gate wddm.c reads once in DriverEntry. The harness exists to exercise the open one. */
unsigned long GuardReadSetting(const wchar_t* Name, unsigned long Default);
unsigned long GuardReadSetting(const wchar_t* Name, unsigned long Default)
{
    if (Name != NULL && wcscmp(Name, L"EnableFullWddm") == 0) return 1;
    return Default;
}

/* The same read, but a 1 on disk is written back as 0 so the gate is one-shot. Nothing is written here:
 * the harness has no registry, and the value it answers is the one the tests are for. 2 rather than 1
 * because 2 is "stay open", which is what a repeated bc250h_start() in one process models. */
unsigned long GuardConsumeSetting(const wchar_t* Name, unsigned long Default);
unsigned long GuardConsumeSetting(const wchar_t* Name, unsigned long Default)
{
    if (Name != NULL && wcscmp(Name, L"EnableFullWddm") == 0) return 2;
    return Default;
}

/* driver\kmd\vram.c. Reached only when Device->VramEnabled is set, and unit A's firmware framebuffer
 * sits at VRAM offset 0 (fact M31: BAR0 shows the same memory from VRAM offset 0). */
unsigned char VramFramebufferOffset(const void* Device, unsigned long long* Offset);
unsigned char VramFramebufferOffset(const void* Device, unsigned long long* Offset)
{
    (void)Device;
    *Offset = 0;
    return 1;
}

/* ---- kernel services --------------------------------------------------------------------------- */

static unsigned char g_Irql;            /* PASSIVE_LEVEL */

void bc250h_stub_set_irql(unsigned char irql) { g_Irql = irql; }

static struct bc250h_stub_counters g_Counters;

void bc250h_stub_counters(struct bc250h_stub_counters* out) { *out = g_Counters; }

static void* Stub_ExAllocatePool2(unsigned long long Flags, size_t NumberOfBytes, unsigned long Tag)
{
    void* p;

    (void)Flags;
    (void)Tag;
    p = malloc(NumberOfBytes);
    if (p == NULL) return NULL;
    memset(p, 0, NumberOfBytes);        /* ExAllocatePool2 zeroes; WddmStart depends on it */
    g_Counters.allocations++;
    g_Counters.bytes += (unsigned long long)NumberOfBytes;
    return p;
}

static void Stub_ExFreePoolWithTag(void* P, unsigned long Tag)
{
    (void)Tag;
    g_Counters.frees++;
    free(P);
}

static void Stub_KeInitializeSpinLock(void* SpinLock) { *(unsigned long long*)SpinLock = 0; }

static unsigned char Stub_KeAcquireSpinLockRaiseToDpc(void* SpinLock)
{
    unsigned char old = g_Irql;

    (void)SpinLock;
    g_Irql = 2;                         /* DISPATCH_LEVEL */
    return old;
}

static void Stub_KeReleaseSpinLock(void* SpinLock, unsigned char NewIrql)
{
    (void)SpinLock;
    g_Irql = NewIrql;
}

static void Stub_KeInitializeDpc(void* Dpc, void* Routine, void* Context)
{
    (void)Routine;
    (void)Context;
    memset(Dpc, 0, 64);                 /* KDPC is 0x40 bytes on x64 */
}

static void Stub_KeInitializeTimerEx(void* Timer, int Type)
{
    (void)Type;
    memset(Timer, 0, 64);               /* KTIMER is 0x40 bytes on x64 */
}

static unsigned char Stub_KeSetTimerEx(void* Timer, long long DueTime, long Period, void* Dpc)
{
    (void)Timer; (void)DueTime; (void)Period; (void)Dpc;
    g_Counters.timer_set++;
    return 0;
}

/* Stage C arms the submit watchdog with the plain KeInitializeTimer / KeSetTimer pair, not the Ex one. */
static void Stub_KeInitializeTimer(void* Timer)
{
    memset(Timer, 0, 64);
}

static unsigned char Stub_KeSetTimer(void* Timer, long long DueTime, void* Dpc)
{
    (void)Timer; (void)DueTime; (void)Dpc;
    g_Counters.timer_set++;
    return 0;
}

/* WddmStop waits out a packet still in flight in 10 ms steps. There is no packet in flight in this
 * process unless a test put one there, and a test that does must not cost the suite half a second, so
 * the wait returns at once and the loop spins its bounded number of times. */
static long Stub_KeDelayExecutionThread(unsigned char WaitMode, unsigned char Alertable, long long* Interval)
{
    (void)WaitMode; (void)Alertable; (void)Interval;
    g_Counters.delays++;
    return 0;                           /* STATUS_SUCCESS */
}

static unsigned char Stub_KeCancelTimer(void* Timer)
{
    (void)Timer;
    g_Counters.timer_cancel++;
    return 0;
}

static unsigned char Stub_KeInsertQueueDpc(void* Dpc, void* Arg1, void* Arg2)
{
    (void)Dpc; (void)Arg1; (void)Arg2;
    g_Counters.dpc_queued++;
    return 1;
}

static unsigned char Stub_KeRemoveQueueDpc(void* Dpc)
{
    (void)Dpc;
    g_Counters.dpc_removed++;
    return 0;
}

static void Stub_KeFlushQueuedDpcs(void) { g_Counters.dpc_flushed++; }

/* 0.7.16's WddmPresentBlit maps the framebuffer and the source allocation. Real memory is handed back
 * rather than NULL, so that the blit walks its copy loop instead of taking the "could not map" exit, and
 * so that a test added later reads defined bytes. The counters pair up: a mapping the driver forgets to
 * release shows as io_maps > io_unmaps. */
static void* Stub_MmMapIoSpaceEx(long long PhysicalAddress, size_t NumberOfBytes, unsigned long Protect)
{
    void* p;

    (void)PhysicalAddress; (void)Protect;
    if (NumberOfBytes == 0) return 0;
    p = calloc(1, NumberOfBytes);
    if (p == 0) return 0;
    g_Counters.io_maps++;
    return p;
}

static void Stub_MmUnmapIoSpace(void* BaseAddress, size_t NumberOfBytes)
{
    (void)NumberOfBytes;
    if (BaseAddress == 0) return;
    g_Counters.io_unmaps++;
    free(BaseAddress);
}

static unsigned char Stub_KeGetCurrentIrql(void) { return g_Irql; }

static long long Stub_KeQueryPerformanceCounter(long long* Frequency)
{
    static long long counter;

    if (Frequency != NULL) *Frequency = 10000000;   /* 10 MHz, like the kernel's own */
    counter += 160000;                              /* one 16 ms VSync period per call */
    return counter;
}

/* The import slots the WDK headers' DECLSPEC_IMPORT declarations make wddm.c call through. */
void* __imp_ExAllocatePool2              = (void*)Stub_ExAllocatePool2;
void* __imp_ExFreePoolWithTag            = (void*)Stub_ExFreePoolWithTag;
void* __imp_KeInitializeSpinLock         = (void*)Stub_KeInitializeSpinLock;
void* __imp_KeAcquireSpinLockRaiseToDpc  = (void*)Stub_KeAcquireSpinLockRaiseToDpc;
void* __imp_KeReleaseSpinLock            = (void*)Stub_KeReleaseSpinLock;
void* __imp_KeInitializeDpc              = (void*)Stub_KeInitializeDpc;
void* __imp_KeInitializeTimerEx          = (void*)Stub_KeInitializeTimerEx;
void* __imp_KeSetTimerEx                 = (void*)Stub_KeSetTimerEx;
void* __imp_KeInitializeTimer            = (void*)Stub_KeInitializeTimer;
void* __imp_KeSetTimer                   = (void*)Stub_KeSetTimer;
void* __imp_KeDelayExecutionThread       = (void*)Stub_KeDelayExecutionThread;
void* __imp_KeCancelTimer                = (void*)Stub_KeCancelTimer;
void* __imp_KeInsertQueueDpc             = (void*)Stub_KeInsertQueueDpc;
void* __imp_KeRemoveQueueDpc             = (void*)Stub_KeRemoveQueueDpc;
void* __imp_KeFlushQueuedDpcs            = (void*)Stub_KeFlushQueuedDpcs;
void* __imp_KeGetCurrentIrql             = (void*)Stub_KeGetCurrentIrql;
void* __imp_KeQueryPerformanceCounter    = (void*)Stub_KeQueryPerformanceCounter;
void* __imp_MmMapIoSpaceEx               = (void*)Stub_MmMapIoSpaceEx;
void* __imp_MmUnmapIoSpace               = (void*)Stub_MmUnmapIoSpace;

/* ---- the display and PnP DDIs the table carries ------------------------------------------------- */

/* WddmBuildTable() stores these 22 addresses and nothing in this harness calls them: the only DDI it
 * enters is DxgkDdiQueryAdapterInfo, which wddm.c owns. Each one aborts rather than return something
 * plausible, so that a future test which does call one finds out instead of measuring a stub. The
 * declarations here carry no arguments on purpose - the x64 ABI needs none to take an address, and
 * repeating 22 kernel signatures in a file that must not see the kernel headers would be a second copy
 * of the DDI contract to keep in step. */
static void Bc250HostStubTrap(const char* name)
{
    printf("FATAL: the host harness called the display DDI %s, which it has no implementation for\n", name);
    exit(2);
}

#define BC250H_DDI_STUB(name)                                                   \
    long name(void);                                                            \
    long name(void) { Bc250HostStubTrap(#name); return (long)0xC00000BBL; }

BC250H_DDI_STUB(Bc250AddDevice)
BC250H_DDI_STUB(Bc250StartDevice)
BC250H_DDI_STUB(Bc250StopDevice)
BC250H_DDI_STUB(Bc250RemoveDevice)
BC250H_DDI_STUB(Bc250ResetDevice)
BC250H_DDI_STUB(Bc250DispatchIoRequest)
BC250H_DDI_STUB(Bc250InterruptRoutine)
BC250H_DDI_STUB(Bc250DpcRoutine)
BC250H_DDI_STUB(Bc250QueryChildRelations)
BC250H_DDI_STUB(Bc250QueryChildStatus)
BC250H_DDI_STUB(Bc250QueryDeviceDescriptor)
BC250H_DDI_STUB(Bc250SetPowerState)
BC250H_DDI_STUB(Bc250Unload)
BC250H_DDI_STUB(Bc250StopDeviceAndReleasePostDisplayOwnership)
BC250H_DDI_STUB(Bc250SetPointerPosition)
BC250H_DDI_STUB(Bc250SetPointerShape)
BC250H_DDI_STUB(Bc250Escape)
BC250H_DDI_STUB(Bc250IsSupportedVidPn)
BC250H_DDI_STUB(Bc250RecommendFunctionalVidPn)
BC250H_DDI_STUB(Bc250EnumVidPnCofuncModality)
BC250H_DDI_STUB(Bc250SetVidPnSourceVisibility)
BC250H_DDI_STUB(Bc250CommitVidPn)
BC250H_DDI_STUB(Bc250UpdateActiveVidPnPresentPath)
BC250H_DDI_STUB(Bc250RecommendMonitorModes)
BC250H_DDI_STUB(Bc250QueryVidPnHWCapability)
BC250H_DDI_STUB(Bc250SystemDisplayEnable)
BC250H_DDI_STUB(Bc250SystemDisplayWrite)
