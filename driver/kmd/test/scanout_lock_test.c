// Host test of the flip target mapping rule under concurrency (BD-289): driver/kmd/scanout_map.h, the same
// ScanoutMapSelect that dcn.c's DcnScanoutMapping runs.
//
// Model. A fake I/O space (the memory manager's part): a fixed number of VA slots, the lowest free slot given out
// first, so that a released VA is reused at once, as the system PTE allocator can. Each mapping records its kind
// (blit source or flip target) and its owner. Releasing a slot that is free is a double release; releasing a slot
// that holds a mapping of another kind is "released another mapping", the BD-289 crash state: the second
// MmUnmapIoSpace of an old flip target hits the PTEs that the other blit's source mapping has meanwhile got.
//
// Workers model WddmPresentBlit: [lock] map the source, ScanoutMapSelect for the current flip target, check that
// the source and the target mapping are still the ones this blit made, [unlock], release the source. A flip
// thread changes the current target between two physical addresses. The workers yield inside the windows that
// matter, so that a missing lock shows in a short run.
//
//   scanout_lock_test.exe            the fixed rule: every blit under the scan-out mutex; must give 0 faults
//   scanout_lock_test.exe --no-lock  the tester.35 shape (no mutex); must give faults (the negative control)
// Exit 0 when the run gives the result its mode expects, 1 otherwise.
#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include "scanout_map.h"

#define SLOTS 64
#define WORKERS 4
#define ITERATIONS 20000
#define SLOT_BYTES 0x01000000ull           // 16 MiB of fake VA a slot
#define FAKE_BASE 0x100000000000ull
#define KIND_SOURCE 1
#define KIND_TARGET 2

typedef struct { int Live, Kind, Owner; unsigned long long Physical; size_t Length; } SLOT;
static SLOT g_Slot[SLOTS];
static CRITICAL_SECTION g_Mm;               // the fake memory manager's own lock
static CRITICAL_SECTION g_ScanoutLock;      // the scan-out mutex of the fix
static volatile LONG g_DoubleRelease, g_ReleasedOther, g_SourceLost, g_TargetLost, g_NoSlot;
static volatile LONG g_Cached, g_Remapped, g_Blits;
static volatile LONG64 g_Current;           // the flip target that DcnCurrentAddress names
static volatile LONG g_Stop;
static int g_UseLock = 1;

// The scan-out state of the device (Device->DcnScanoutMap, ...Address, ...Length).
static void* g_Map;
static unsigned long long g_MapPhysical;
static size_t g_MapLength;

typedef struct { int Kind, Owner; } MAP_CONTEXT;

static void* FakeMap(int Kind, int Owner, unsigned long long Physical, size_t Length)
{
    int i;
    void* va = NULL;
    EnterCriticalSection(&g_Mm);
    for (i = 0; i < SLOTS; i++)
        if (!g_Slot[i].Live)
        {
            g_Slot[i].Live = 1; g_Slot[i].Kind = Kind; g_Slot[i].Owner = Owner;
            g_Slot[i].Physical = Physical; g_Slot[i].Length = Length;
            va = (void*)(ULONG_PTR)(FAKE_BASE + (unsigned long long)i * SLOT_BYTES);
            break;
        }
    LeaveCriticalSection(&g_Mm);
    if (va == NULL) InterlockedIncrement(&g_NoSlot);
    return va;
}

static int SlotOf(const void* Va)
{
    unsigned long long a = (unsigned long long)(ULONG_PTR)Va;
    if (a < FAKE_BASE || (a - FAKE_BASE) % SLOT_BYTES != 0) return -1;
    a = (a - FAKE_BASE) / SLOT_BYTES;
    return a < SLOTS ? (int)a : -1;
}

static void FakeUnmap(int Kind, const void* Va)
{
    int i = SlotOf(Va);
    EnterCriticalSection(&g_Mm);
    if (i < 0 || !g_Slot[i].Live) InterlockedIncrement(&g_DoubleRelease);
    else
    {
        if (g_Slot[i].Kind != Kind) InterlockedIncrement(&g_ReleasedOther);
        g_Slot[i].Live = 0;
    }
    LeaveCriticalSection(&g_Mm);
}

static int StillMine(const void* Va, int Kind, int Owner, unsigned long long Physical)
{
    int i = SlotOf(Va), ok;
    EnterCriticalSection(&g_Mm);
    ok = i >= 0 && g_Slot[i].Live && g_Slot[i].Kind == Kind && g_Slot[i].Physical == Physical &&
         (Kind != KIND_SOURCE || g_Slot[i].Owner == Owner);
    LeaveCriticalSection(&g_Mm);
    return ok;
}

static void* OpsMap(void* Context, unsigned long long Physical, size_t Length)
{
    MAP_CONTEXT* c = (MAP_CONTEXT*)Context;
    SwitchToThread();                       // a window between the release of the old target and the new map
    return FakeMap(KIND_TARGET, c->Owner, Physical, Length);
}

static void OpsUnmap(void* Context, void* Mapping, size_t Length)
{
    (void)Context; (void)Length;
    FakeUnmap(KIND_TARGET, Mapping);
    SwitchToThread();
}

static int OpsAllowed(void* Context, unsigned long long Physical)
{
    (void)Context;
    return Physical != 0;
}

static DWORD WINAPI Worker(void* Parameter)
{
    int me = (int)(ULONG_PTR)Parameter, n;
    MAP_CONTEXT c = { KIND_TARGET, me };
    BC250_SCANOUT_MAP_OPS ops = { &c, OpsMap, OpsUnmap, OpsAllowed, NULL, NULL, NULL };
    for (n = 0; n < ITERATIONS; n++)
    {
        unsigned long long source = 0x2808CA000ull + (unsigned long long)me * 0x01000000ull;
        unsigned long long target;
        void* src;
        int result;
        if (g_UseLock) EnterCriticalSection(&g_ScanoutLock);
        src = FakeMap(KIND_SOURCE, me, source, 0x8CA000);
        target = (unsigned long long)InterlockedCompareExchange64(&g_Current, 0, 0);
        result = ScanoutMapSelect(&g_Map, &g_MapPhysical, &g_MapLength, target, 0x8CA000, &ops);
        if (result == BC250_SCANOUT_MAP_CACHED) InterlockedIncrement(&g_Cached);
        if (result == BC250_SCANOUT_MAP_NEW) InterlockedIncrement(&g_Remapped);
        if (result == BC250_SCANOUT_MAP_CACHED || result == BC250_SCANOUT_MAP_NEW)
        {
            void* dst = g_Map;              // the mapping the copy would write into
            SwitchToThread();               // the copy
            if (!StillMine(dst, KIND_TARGET, me, target)) InterlockedIncrement(&g_TargetLost);
        }
        if (src != NULL && !StillMine(src, KIND_SOURCE, me, source)) InterlockedIncrement(&g_SourceLost);
        if (g_UseLock) LeaveCriticalSection(&g_ScanoutLock);
        if (src != NULL) FakeUnmap(KIND_SOURCE, src);
        InterlockedIncrement(&g_Blits);
    }
    return 0;
}

static DWORD WINAPI Flipper(void* Parameter)
{
    (void)Parameter;
    while (!g_Stop)
    {
        LONG64 now = InterlockedCompareExchange64(&g_Current, 0, 0);
        InterlockedExchange64(&g_Current, now == 0x280000000ll ? 0x2808CA000ll : 0x280000000ll);
        Sleep(0);
    }
    return 0;
}

int main(int argc, char** argv)
{
    HANDLE t[WORKERS], f;
    int i, faults, pass;
    BC250_SCANOUT_MAP_OPS release;
    MAP_CONTEXT rc = { KIND_TARGET, -1 };
    if (argc > 1 && !strcmp(argv[1], "--no-lock")) g_UseLock = 0;
    InitializeCriticalSection(&g_Mm);
    InitializeCriticalSection(&g_ScanoutLock);
    g_Current = 0x280000000ll;
    f = CreateThread(NULL, 0, Flipper, NULL, 0, NULL);
    for (i = 0; i < WORKERS; i++) t[i] = CreateThread(NULL, 0, Worker, (void*)(ULONG_PTR)i, 0, NULL);
    WaitForMultipleObjects(WORKERS, t, TRUE, 120000);
    g_Stop = 1;
    WaitForSingleObject(f, 10000);
    // The stop path (DcnUnmapScanout): one release of what is left, then every slot must be free.
    release.Context = &rc; release.Map = OpsMap; release.Unmap = OpsUnmap; release.Allowed = OpsAllowed;
    release.BeforeRemap = NULL; release.AfterUnmap = NULL; release.AfterMap = NULL;
    ScanoutMapRelease(&g_Map, &g_MapPhysical, &g_MapLength, &release);
    {
        int leaked = 0;
        for (i = 0; i < SLOTS; i++) leaked += g_Slot[i].Live;
        faults = g_DoubleRelease + g_ReleasedOther + g_SourceLost + g_TargetLost + g_NoSlot + (g_UseLock ? leaked : 0);
        printf("mode %s: blits %ld cached %ld remapped %ld | double-release %ld released-other %ld source-lost %ld "
               "target-lost %ld no-slot %ld leaked %d\n", g_UseLock ? "lock" : "no-lock", g_Blits, g_Cached,
               g_Remapped, g_DoubleRelease, g_ReleasedOther, g_SourceLost, g_TargetLost, g_NoSlot, leaked);
    }
    if (g_UseLock)
    {
        pass = faults == 0 && g_Cached > 0 && g_Remapped > 0 && g_Blits == WORKERS * ITERATIONS;
        printf("scanout-lock %s: the fixed rule, %d fault(s)\n", pass ? "PASS" : "FAIL", faults);
    }
    else
    {
        pass = faults > 0;
        printf("scanout-lock %s: without the mutex the model %s faults (negative control)\n", pass ? "PASS" : "FAIL",
               pass ? "shows" : "shows no");
    }
    return pass ? 0 : 1;
}
