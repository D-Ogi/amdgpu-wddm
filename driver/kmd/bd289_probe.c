// BD-289 diagnostic probes of the CPU present blit's source mapping. Lab flavour only; the contract is in
// bd289_probe.h. The release flavour compiles this file to nothing.
#include "bc250kmd.h"
#include "bd289_probe.h"

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)

#define BD289_REMAP_LINES 24            // 4 lines a probe with a remap: the first 6 such probes
#define BD289_SUM_LINES 80              // 2 lines a summary: 40 summaries

typedef struct {
    volatile LONG Busy;                 // 1 while a present holds the probe
    PKTHREAD Owner;                     // the thread of that present; NULL outside Begin..End
    const UCHAR* Map;
    SIZE_T Length;
    ULONGLONG Physical;
    ULONG Protect;
    LONG Index;                         // 1 for the first probe of this driver load
    UCHAR State[BD289_STEPS];
    ULONG BadCopy, BadEnd;              // invalid or foreign pages of the whole range at COPY and END
    ULONG LostAt;                       // the first step that was not BD289_STATE_GOOD; BD289_STEPS when none
    BOOLEAN Remapped, PhysOverlap, VaOverlap;
    LONG RemapIndex;
    const void* OldMap;
    ULONGLONG OldPhysical;
    SIZE_T OldLength;
    const void* NewMap;
    ULONGLONG NewPhysical;
    SIZE_T NewLength;
} BD289_PROBE;

static BD289_PROBE g_Probe;
static volatile LONG g_Probes, g_BusySkips, g_Remaps, g_ProbeRemaps, g_PhysOverlaps, g_VaOverlaps, g_Foreign;
static volatile LONG g_Lost[BD289_STEPS], g_UnmapKept, g_CopySkipped;
static volatile LONG g_InfoLines, g_RemapLines, g_LostLines, g_SumLines;

static BOOLEAN Budget(volatile LONG* Used, LONG Lines, LONG Limit)
{
    return InterlockedExchangeAdd(Used, Lines) + Lines <= Limit;
}

static BOOLEAN Mine(void)
{
    return g_Probe.Busy != 0 && g_Probe.Owner == KeGetCurrentThread();
}

static BOOLEAN PageGood(const UCHAR* Va, ULONGLONG Physical)
{
    return MmIsAddressValid((PVOID)Va) && (ULONGLONG)MmGetPhysicalAddress((PVOID)Va).QuadPart == Physical;
}

static UCHAR ProbeState(_Out_opt_ ULONG* Bad)
{
    const BD289_PROBE* p = &g_Probe;
    SIZE_T lastOffset = (p->Length - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    UCHAR s = BD289_STATE_REACHED;
    if (MmIsAddressValid((PVOID)p->Map))
    {
        s |= BD289_STATE_FIRST;
        if ((ULONGLONG)MmGetPhysicalAddress((PVOID)p->Map).QuadPart == p->Physical) s |= BD289_STATE_PHYSICAL;
    }
    if (p->Length <= PAGE_SIZE || MmIsAddressValid((PVOID)(p->Map + PAGE_SIZE))) s |= BD289_STATE_SECOND;
    if (MmIsAddressValid((PVOID)(p->Map + lastOffset))) s |= BD289_STATE_LAST;
    if (Bad != NULL)
    {
        SIZE_T offset;
        ULONG bad = 0;
        for (offset = 0; offset < p->Length; offset += PAGE_SIZE)
            if (!PageGood(p->Map + offset, p->Physical + offset)) bad++;
        *Bad = bad;
    }
    return s;
}

BOOLEAN Bd289ProbeBegin(_In_ const void* Map, SIZE_T Length, ULONGLONG Physical, ULONG Protect)
{
    if (Map == NULL || Length == 0) return FALSE;
    if (InterlockedCompareExchange(&g_Probe.Busy, 1, 0) != 0)
    {
        InterlockedIncrement(&g_BusySkips);
        return FALSE;
    }
    g_Probe.Map = (const UCHAR*)Map;
    g_Probe.Length = Length;
    g_Probe.Physical = Physical;
    g_Probe.Protect = Protect;
    RtlZeroMemory(g_Probe.State, sizeof(g_Probe.State));
    g_Probe.BadCopy = g_Probe.BadEnd = 0;
    g_Probe.LostAt = BD289_STEPS;
    g_Probe.Remapped = g_Probe.PhysOverlap = g_Probe.VaOverlap = FALSE;
    g_Probe.RemapIndex = 0;
    g_Probe.OldMap = g_Probe.NewMap = NULL;
    g_Probe.OldPhysical = g_Probe.NewPhysical = 0;
    g_Probe.OldLength = g_Probe.NewLength = 0;
    g_Probe.Index = InterlockedIncrement(&g_Probes);
    KeMemoryBarrier();
    g_Probe.Owner = KeGetCurrentThread();       // last: until here no other thread can match it
    (void)Bd289ProbeStep(BD289_STEP_MAP);
    return TRUE;
}

BOOLEAN Bd289ProbeStep(ULONG Step)
{
    UCHAR s;
    ULONG bad = 0;
    BOOLEAN whole = (Step == BD289_STEP_COPY || Step == BD289_STEP_END);
    if (Step >= BD289_STEPS) return TRUE;
    if (!Mine())
    {
        if (g_Probe.Busy) InterlockedIncrement(&g_Foreign);
        return TRUE;
    }
    s = ProbeState(whole ? &bad : NULL);
    g_Probe.State[Step] = s;
    if (Step == BD289_STEP_COPY) g_Probe.BadCopy = bad;
    if (Step == BD289_STEP_END) g_Probe.BadEnd = bad;
    if (((s & BD289_STATE_GOOD) != BD289_STATE_GOOD || bad != 0))
    {
        if (g_Probe.LostAt == BD289_STEPS)
        {
            g_Probe.LostAt = Step;
            InterlockedIncrement(&g_Lost[Step]);
        }
        if (Step == BD289_STEP_COPY) InterlockedIncrement(&g_CopySkipped);
        return FALSE;
    }
    return TRUE;
}

void Bd289ProbeRemap(_In_opt_ const void* OldMap, ULONGLONG OldPhysical, SIZE_T OldLength,
                     ULONGLONG NewPhysical, SIZE_T NewLength)
{
    LONG n = InterlockedIncrement(&g_Remaps);
    if (Mine())
    {
        const UCHAR* old = (const UCHAR*)OldMap;
        g_Probe.Remapped = TRUE;
        g_Probe.RemapIndex = n;
        g_Probe.OldMap = OldMap;
        g_Probe.OldPhysical = OldPhysical;
        g_Probe.OldLength = OldLength;
        g_Probe.NewPhysical = NewPhysical;
        g_Probe.NewLength = NewLength;
        g_Probe.PhysOverlap = OldMap != NULL && OldLength != 0 &&
            OldPhysical < g_Probe.Physical + g_Probe.Length && g_Probe.Physical < OldPhysical + OldLength;
        g_Probe.VaOverlap = old != NULL && OldLength != 0 &&
            old < g_Probe.Map + g_Probe.Length && g_Probe.Map < old + OldLength;
        InterlockedIncrement(&g_ProbeRemaps);
        if (g_Probe.PhysOverlap) InterlockedIncrement(&g_PhysOverlaps);
        if (g_Probe.VaOverlap) InterlockedIncrement(&g_VaOverlaps);
        return;
    }
    if (g_Probe.Busy) InterlockedIncrement(&g_Foreign);
    if (Budget(&g_RemapLines, 1, BD289_REMAP_LINES))
        GuardLog("bd289: remap %ld no probe old %p %llX+%llX new %llX+%llX", n,
                 (void*)OldMap, OldPhysical, (ULONGLONG)OldLength, NewPhysical, (ULONGLONG)NewLength);
}

void Bd289ProbeRemapped(_In_opt_ const void* NewMap)
{
    if (Mine()) g_Probe.NewMap = NewMap;
}

static void Summary(void)
{
    GuardLog("bd289: sum probes %ld busy %ld remaps %ld in-probe %ld phys-overlap %ld va-overlap %ld foreign %ld",
             g_Probes, g_BusySkips, g_Remaps, g_ProbeRemaps, g_PhysOverlaps, g_VaOverlaps, g_Foreign);
    GuardLog("bd289: lost by step %ld %ld %ld %ld %ld %ld %ld, unmap kept %ld, copy skipped %ld",
             g_Lost[BD289_STEP_MAP], g_Lost[BD289_STEP_UNMAP], g_Lost[BD289_STEP_REMAP], g_Lost[BD289_STEP_SCANOUT],
             g_Lost[BD289_STEP_DEST], g_Lost[BD289_STEP_COPY], g_Lost[BD289_STEP_END], g_UnmapKept, g_CopySkipped);
}

BOOLEAN Bd289ProbeEnd(void)
{
    BOOLEAN safe, lost, write;
    LONG index;
    if (!Mine()) return TRUE;
    safe = Bd289ProbeStep(BD289_STEP_END);
    lost = g_Probe.LostAt != BD289_STEPS;
    index = g_Probe.Index;
    if (!safe) InterlockedIncrement(&g_UnmapKept);
    if (lost) write = Budget(&g_LostLines, 4, BD289_LOST_LINES);
    else if (g_Probe.Remapped) write = Budget(&g_RemapLines, 4, BD289_REMAP_LINES);
    else write = (index <= 4 || (index & (index - 1)) == 0) && Budget(&g_InfoLines, 2, BD289_INFO_LINES);
    if (write)
    {
        const BD289_PROBE* p = &g_Probe;
        GuardLog("bd289: probe %ld map %p len %llX phys %llX prot %lX%s", index, (void*)p->Map,
                 (ULONGLONG)p->Length, p->Physical, p->Protect, lost ? " LOST" : "");
        GuardLog("bd289: probe %ld steps %lX %lX %lX %lX %lX %lX %lX bad %lu/%lu lost-at %lu", index,
                 (ULONG)p->State[0], (ULONG)p->State[1], (ULONG)p->State[2], (ULONG)p->State[3],
                 (ULONG)p->State[4], (ULONG)p->State[5], (ULONG)p->State[6], p->BadCopy, p->BadEnd, p->LostAt);
        if (p->Remapped)
        {
            GuardLog("bd289: probe %ld remap %ld old %p phys %llX len %llX", index, p->RemapIndex,
                     (void*)p->OldMap, p->OldPhysical, (ULONGLONG)p->OldLength);
            GuardLog("bd289: probe %ld remap %ld new %p phys %llX len %llX overlap phys %lu va %lu", index,
                     p->RemapIndex, (void*)p->NewMap, p->NewPhysical, (ULONGLONG)p->NewLength,
                     (ULONG)p->PhysOverlap, (ULONG)p->VaOverlap);
        }
    }
    // The counters, with every group written and every 1024 probes, from a budget of their own, so that a late read
    // of the log still finds counters close to the end of the run.
    if ((write || (index & 1023) == 0) && Budget(&g_SumLines, 2, BD289_SUM_LINES)) Summary();
    g_Probe.Owner = NULL;
    KeMemoryBarrier();
    InterlockedExchange(&g_Probe.Busy, 0);
    return safe;
}

#endif
