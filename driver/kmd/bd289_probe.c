// BD-289 diagnostic probes of the CPU present blit's source mapping. Lab flavour only; the contract is in
// bd289_probe.h. The release flavour compiles this file to nothing.
#include "bc250kmd.h"
#include "bd289_probe.h"
#include <ntstrsafe.h>

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)

#define BD289_REMAP_LINES 24            // remap lines in the log ring outside the concurrent ones: the first remaps
#define BD289_CONC_LINES 48             // lines in the log ring for concurrent blits and remaps during another blit
#define BD289_SUM_LINES 80              // 3 lines a summary
#define BD289_RING_LINES 128            // the bd289 ring: the first BD289_RING_HEAD lines of the load are kept,
#define BD289_RING_HEAD 32              // the other 96 rotate
#define BD289_RING_TEXT 152

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
static volatile LONG g_InfoLines, g_RemapLines, g_LostLines, g_SumLines, g_ConcLines;
// r3: the blits on their way (before any lock) and inside the locked section.
static volatile LONG g_Arriving, g_Arrivals, g_ArriveConcurrent, g_ArriveMax;
static volatile LONG g_Inside, g_Entries, g_EnterConcurrent, g_EnterMax, g_Unlocked, g_RemapConcurrent;
// The bd289 ring (BD289_RING_HEAD kept, the rest rotating). A zeroed KSPIN_LOCK is a free one.
static KSPIN_LOCK g_RingLock;
static ULONG g_RingNext;
static ULONG g_RingSeq[BD289_RING_LINES];
static char g_Ring[BD289_RING_LINES][BD289_RING_TEXT];

static BOOLEAN Budget(volatile LONG* Used, LONG Lines, LONG Limit)
{
    return InterlockedExchangeAdd(Used, Lines) + Lines <= Limit;
}

static ULONG RingSlot(ULONG Seq)
{
    return Seq < BD289_RING_LINES ? Seq
                                  : BD289_RING_HEAD + (Seq - BD289_RING_HEAD) % (BD289_RING_LINES - BD289_RING_HEAD);
}

// One bd289 line: always into the bd289 ring, into the log ring when ToLog. The formats of the callers are at most
// 159 characters wide by the rules of tools/quality/guardlog_width.py (checked by hand: GuardLog sees only "%s").
static void Emit(BOOLEAN ToLog, _In_z_ _Printf_format_string_ const char* Format, ...)
{
    char line[BC250_LOG_TEXT];
    va_list args;
    KIRQL irql;
    ULONG seq, slot, skip;

    va_start(args, Format);
    (void)RtlStringCbVPrintfA(line, sizeof(line), Format, args);
    va_end(args);
    skip = (RtlCompareMemory(line, "bd289: ", 7) == 7) ? 7 : 0;
    KeAcquireSpinLock(&g_RingLock, &irql);
    seq = g_RingNext++;
    slot = RingSlot(seq);
    g_RingSeq[slot] = seq;
    (void)RtlStringCbCopyA(g_Ring[slot], BD289_RING_TEXT, line + skip);
    KeReleaseSpinLock(&g_RingLock, irql);
    if (ToLog) GuardLog("%s", line);
}

static BOOLEAN Mine(void)
{
    return g_Probe.Busy != 0 && g_Probe.Owner == KeGetCurrentThread();
}

static void Max(volatile LONG* Target, LONG Value)
{
    LONG old = *Target;
    while (Value > old)
    {
        LONG seen = InterlockedCompareExchange(Target, Value, old);
        if (seen == old) break;
        old = seen;
    }
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

void Bd289BlitArrive(void)
{
    LONG active = InterlockedIncrement(&g_Arriving);
    InterlockedIncrement(&g_Arrivals);
    Max(&g_ArriveMax, active);
    if (active > 1) InterlockedIncrement(&g_ArriveConcurrent);
}

void Bd289BlitEnter(BOOLEAN Locked)
{
    LONG active = InterlockedIncrement(&g_Inside);
    LONG n = InterlockedIncrement(&g_Entries);
    Max(&g_EnterMax, active);
    if (!Locked) InterlockedIncrement(&g_Unlocked);
    if (active > 1)
    {
        LONG c = InterlockedIncrement(&g_EnterConcurrent);
        Emit(Budget(&g_ConcLines, 1, BD289_CONC_LINES), "bd289: blit %ld enters while %ld inside, locked %lu, concurrent %ld",
             n, active - 1, (ULONG)Locked, c);
    }
}

void Bd289BlitLeave(void)
{
    (void)InterlockedDecrement(&g_Inside);
}

void Bd289BlitDepart(void)
{
    (void)InterlockedDecrement(&g_Arriving);
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
    LONG inside = InterlockedCompareExchange(&g_Inside, 0, 0);
    BOOLEAN mine = Mine();
    BOOLEAN concurrent = inside > 1;
    if (concurrent) InterlockedIncrement(&g_RemapConcurrent);
    // Every remap into the bd289 ring; into the log ring the first ones and every remap during another blit.
    Emit(concurrent ? Budget(&g_ConcLines, 1, BD289_CONC_LINES) : Budget(&g_RemapLines, 1, BD289_REMAP_LINES),
         "bd289: remap %ld old %p %llX+%lX new %llX+%lX inside %ld probe %lu", n, (void*)OldMap, OldPhysical,
         (ULONG)OldLength, NewPhysical, (ULONG)NewLength, inside, (ULONG)mine);
    if (mine)
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
}

void Bd289ProbeRemapped(_In_opt_ const void* NewMap)
{
    LONG inside = InterlockedCompareExchange(&g_Inside, 0, 0);
    Emit(FALSE, "bd289: remap %ld new map %p inside %ld", InterlockedCompareExchange(&g_Remaps, 0, 0),
         (void*)NewMap, inside);
    if (Mine()) g_Probe.NewMap = NewMap;
}

static void Summary(BOOLEAN ToLog)
{
    Emit(ToLog, "bd289: sum probes %ld busy %ld remaps %ld in-probe %ld phys-overlap %ld va-overlap %ld foreign %ld",
         g_Probes, g_BusySkips, g_Remaps, g_ProbeRemaps, g_PhysOverlaps, g_VaOverlaps, g_Foreign);
    Emit(ToLog, "bd289: lost by step %ld %ld %ld %ld %ld %ld %ld, unmap kept %ld, copy skipped %ld",
         g_Lost[BD289_STEP_MAP], g_Lost[BD289_STEP_UNMAP], g_Lost[BD289_STEP_REMAP], g_Lost[BD289_STEP_SCANOUT],
         g_Lost[BD289_STEP_DEST], g_Lost[BD289_STEP_COPY], g_Lost[BD289_STEP_END], g_UnmapKept, g_CopySkipped);
    Emit(ToLog, "bd289: blits %ld arrive-conc %ld max %ld inside-conc %ld max %ld unlocked %ld remap-conc %ld",
         g_Arrivals, g_ArriveConcurrent, g_ArriveMax, g_EnterConcurrent, g_EnterMax, g_Unlocked, g_RemapConcurrent);
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
    // Into the bd289 ring: every lost probe, every probe with a remap, and the probes the log ring gets.
    if (write || lost || g_Probe.Remapped)
    {
        const BD289_PROBE* p = &g_Probe;
        Emit(write, "bd289: probe %ld map %p len %llX phys %llX prot %lX%s", index, (void*)p->Map,
             (ULONGLONG)p->Length, p->Physical, p->Protect, lost ? " LOST" : "");
        Emit(write, "bd289: probe %ld steps %lX %lX %lX %lX %lX %lX %lX bad %lu/%lu lost-at %lu", index,
             (ULONG)p->State[0], (ULONG)p->State[1], (ULONG)p->State[2], (ULONG)p->State[3],
             (ULONG)p->State[4], (ULONG)p->State[5], (ULONG)p->State[6], p->BadCopy, p->BadEnd, p->LostAt);
        if (p->Remapped)
        {
            Emit(write, "bd289: probe %ld remap %ld old %p phys %llX len %llX", index, p->RemapIndex,
                 (void*)p->OldMap, p->OldPhysical, (ULONGLONG)p->OldLength);
            Emit(write, "bd289: probe %ld remap %ld new %p phys %llX len %llX overlap phys %lu va %lu", index,
                 p->RemapIndex, (void*)p->NewMap, p->NewPhysical, (ULONGLONG)p->NewLength,
                 (ULONG)p->PhysOverlap, (ULONG)p->VaOverlap);
        }
    }
    // The counters, with every group written and every 1024 probes, from a budget of their own, so that a late read
    // of the log still finds counters close to the end of the run.
    if ((write || (index & 1023) == 0) && Budget(&g_SumLines, 3, BD289_SUM_LINES)) Summary(TRUE);
    g_Probe.Owner = NULL;
    KeMemoryBarrier();
    InterlockedExchange(&g_Probe.Busy, 0);
    return safe;
}

// The summary block (`bc250kmd_cli log summary only`): the counters now, then the bd289 ring in sequence order,
// one "bd289 r<seq> <text>" line each. A line that rotated out while this runs is left out, not printed twice.
void Bd289ProbeSummary(void)
{
    char text[BD289_RING_TEXT];
    ULONG count, seq, first, slot;
    KIRQL irql;
    BOOLEAN have;

    Summary(FALSE);
    for (seq = 0; seq < BD289_RING_LINES; seq++)
    {
        slot = RingSlot(seq);
        KeAcquireSpinLock(&g_RingLock, &irql);
        count = g_RingNext;
        have = (seq < count) && g_RingSeq[slot] == seq;
        if (have) (void)RtlStringCbCopyA(text, sizeof(text), g_Ring[slot]);
        KeReleaseSpinLock(&g_RingLock, irql);
        if (seq >= count) break;
        if (seq == BD289_RING_HEAD) break;          // the head ends here; the tail follows below
        if (have) GuardLog("bd289 r%lu %s", seq, text);
    }
    KeAcquireSpinLock(&g_RingLock, &irql);
    count = g_RingNext;
    KeReleaseSpinLock(&g_RingLock, irql);
    first = count > BD289_RING_LINES ? count - (BD289_RING_LINES - BD289_RING_HEAD) : BD289_RING_HEAD;
    if (count > BD289_RING_HEAD && first > BD289_RING_HEAD)
        GuardLog("bd289 ring: %lu lines rotated out between r%lu and r%lu", first - BD289_RING_HEAD,
                 (ULONG)BD289_RING_HEAD, first);
    for (seq = first; seq < count; seq++)
    {
        slot = RingSlot(seq);
        KeAcquireSpinLock(&g_RingLock, &irql);
        have = g_RingSeq[slot] == seq;
        if (have) (void)RtlStringCbCopyA(text, sizeof(text), g_Ring[slot]);
        KeReleaseSpinLock(&g_RingLock, irql);
        if (have) GuardLog("bd289 r%lu %s", seq, text);
    }
    GuardLog("bd289 ring: %lu lines written, summary done", count);
}

#endif
