// Progress records for hang evidence (hang.c, docs/design/hang-detector.md). One static instance in the driver
// image, written with interlocked operations only, read from a kernel dump by its signature. No WDK type here:
// the layout and the two pure decisions below are compiled on the host by test/hang_progress_test.c as well.
#pragma once

#define BC250_PROGRESS_SIGNATURE 0x4752503035324342ull  // "BC250PRG" in memory order
#define BC250_PROGRESS_VERSION 2u                        // 2: per-site Input at entry, IH per-slot sequence
#define BC250_PROGRESS_IH_RECENT 16u                     // power of two: the slot is a mask of a counter
#define BC250_PROGRESS_IH_SOURCES 256u                   // src_id is eight bits; clients share a slot

// Each site's Value is written at its exit (at its entry for the ISR, which has no other moment worth a store).
typedef enum BC250_PROGRESS_SITE_ID {
    ProgressSiteIsr = 0,                // Bc250InterruptRoutine; Value: message number
    ProgressSiteDeviceDpc,              // Bc250DpcRoutine
    ProgressSiteIhDpc,                  // IhDpc; Value: Consume passes of that invocation
    ProgressSitePagingDrain,            // WddmGpuFencePaging; Value: PAGING_DRAIN_EXIT
    ProgressSitePagingSubmit,           // GfxSubmitPaging; Value: sequence, 0 when refused
    ProgressSiteGfxSubmit,              // GfxSubmitIb, GartLock wait included; Value: sequence, 0 when refused
    ProgressSiteVmFlush,                // bc250_gmc_set_vmid_pd in the graphics submit; Input: the VMID; Value: its result
    ProgressSiteReportDpc,              // WddmReportDpcRoutine
    ProgressSiteSubmitWatchdogDpc,      // node 0's submit watchdog: it now ticks inside the budget and re-arms
                                        // itself while a head exists (BD-114, submit_watchdog.h), so Entries
                                        // grows by about 4 a second under load and says nothing about a hang
    ProgressSitePagingWatchdogDpc,      // node 1's deadline timer, one shot at SubmitBudgetMs
    ProgressSitePagingDrainDpc,         // node 1's quota requeue timer
    ProgressSiteVSyncDpc,               // the software VSync timer (flip gate closed)
    ProgressSiteBuildPagingBuffer,      // PagingBuildLock wait included; Input and Value: DXGK_BUILDPAGINGBUFFER_OPERATION
    ProgressSiteSubmitCommand,          // Input and Value: SubmissionFenceId
    ProgressSiteSubmitCommandVirtual,   // Input and Value: SubmissionFenceId
    ProgressSiteCount
} BC250_PROGRESS_SITE_ID;

// Fields are sampled independently, not as one snapshot of one invocation. Input is the argument of the most
// recent entry, written before Entries counts it; Value is the payload of the most recent exit. With a call
// stalled inside, Value still describes an earlier completed call, and with concurrent callers Input may belong
// to a later entry than the stalled one. Neither names the open call without further evidence; neither does
// LastCpu.
typedef struct BC250_PROGRESS_SITE {
    volatile long Entries, Exits;       // Entries != Exits: somebody is inside, or was when the dump was taken
    volatile long LastCpu;              // processor index of the last entry
    volatile long Value;                // exit payload (see the site list)
    volatile long Input;                // entry argument, 0 for sites without one
    volatile long Reserved;             // keeps the times 8-byte aligned without implicit padding
    volatile long long LastEntryTime;   // KeQueryInterruptTime, 100 ns, same clock as the device's own samples
    volatile long long LastExitTime;
} BC250_PROGRESS_SITE;

typedef struct BC250_PROGRESS_DRAIN {
    volatile long Iterations;           // passes of the drain loop, all invocations
    volatile long Retired;              // jobs retired
    volatile long QuotaExits;           // invocations that stopped at PAGING_DRAIN_QUOTA
    volatile long LastIterations;       // passes of the most recent finished invocation
    volatile long MaxIterations;        // the longest finished invocation
    volatile long LastExit;             // PAGING_DRAIN_EXIT of the most recent finished invocation
} BC250_PROGRESS_DRAIN;

typedef struct BC250_PROGRESS_IH {
    volatile long Vectors;              // decoded vectors
    volatile long Requeues;             // Consume budget exhausted, DPC queued again
    volatile long Handoffs;             // Consume passes beyond the first in one IhDpc (the DpcAgain handoff)
    // Recent is best-effort. RecentNext counts reserved slots and may run ahead of what was written: a crash
    // between reservation and commit leaves a slot with its old contents. A slot is valid when its RecentSeq is
    // nonzero; RecentSeq is the 1-based vector number, cleared while the slot is rewritten and set after it.
    volatile long RecentNext;           // slots reserved; slot of vector n (1-based) = (n - 1) mod 16
    volatile long Recent[BC250_PROGRESS_IH_RECENT];     // (client_id << 8) | src_id
    volatile long RecentSeq[BC250_PROGRESS_IH_RECENT];  // 0: being written or never written; else vector number
    volatile long BySource[BC250_PROGRESS_IH_SOURCES];  // vectors per src_id
} BC250_PROGRESS_IH;

typedef struct BC250_PROGRESS_WATCH {
    volatile long Armed;                // EnableHangBugcheck was 1 at this device start: thread and timer exist
    volatile long Active;               // the timer may judge: started, D0, not stopping
    volatile long LimitSeconds;         // HangBugcheckSeconds as accepted
    volatile long Heartbeat;            // the thread's counter
    volatile long Checks;               // timer DPC runs
    volatile long Fired;                // set just before KeBugCheckEx
    // Watch times use the unbiased clock (sleep excluded); site times use KeQueryInterruptTime (sleep included).
    // The two domains differ after any sleep: never subtract one from the other.
    volatile long long HeartbeatTime;   // KeQueryUnbiasedInterruptTime of the last heartbeat, 100 ns
    volatile long long CheckTime;       // the same unbiased clock, the last timer DPC
} BC250_PROGRESS_WATCH;

typedef struct BC250_PROGRESS {
    unsigned long long Signature;       // BC250_PROGRESS_SIGNATURE, set at compile time: present from load on
    unsigned long Version;              // BC250_PROGRESS_VERSION: this layout
    unsigned long Size;                 // sizeof(BC250_PROGRESS)
    unsigned long KmdVersion;           // BC250_KMD_VERSION of the image
    unsigned long SiteCount;            // ProgressSiteCount
    BC250_PROGRESS_SITE Sites[ProgressSiteCount];
    BC250_PROGRESS_DRAIN Drain;
    BC250_PROGRESS_IH Ih;
    BC250_PROGRESS_WATCH Watch;
} BC250_PROGRESS;

// ---- the hang detector's decision -------------------------------------------------------------------------------

#define BC250_HANG_SECONDS_DEFAULT 10u
#define BC250_HANG_SECONDS_MIN 5u
#define BC250_HANG_SECONDS_MAX 60u

// HangBugcheckSeconds outside 5..60 is not clamped to the nearer end: a typo should not arm a 5 s trigger.
static __inline unsigned long Bc250HangSeconds(unsigned long Requested)
{
    return (Requested >= BC250_HANG_SECONDS_MIN && Requested <= BC250_HANG_SECONDS_MAX) ?
        Requested : BC250_HANG_SECONDS_DEFAULT;
}

typedef struct BC250_HANG_WATCH_STATE {
    long Heartbeat;                     // the value seen at Advanced
    unsigned long long Advanced;        // when the heartbeat was last seen to change
    unsigned long long Checked;         // the previous check
    int Primed;
} BC250_HANG_WATCH_STATE;

// One check of the timer DPC; times in 100 ns. Nonzero when the heartbeat has not moved for Limit while the
// checks themselves kept watching: a gap of Limit or more between two checks (the DPC was not running, a
// debugger held the machine) starts a new window instead of counting as stale, so what fires is a heartbeat that
// was watched standing still, not one nobody looked at. *Age is the stale time, 0 when it moved.
static __inline int Bc250HangWatchCheck(BC250_HANG_WATCH_STATE* State, long Heartbeat,
                                        unsigned long long Now, unsigned long long Limit,
                                        unsigned long long* Age)
{
    int watched = State->Primed && Now >= State->Checked && Now - State->Checked < Limit;
    State->Checked = Now;
    *Age = 0;
    if (!watched || Heartbeat != State->Heartbeat || Now < State->Advanced)
    {
        State->Primed = 1;
        State->Heartbeat = Heartbeat;
        State->Advanced = Now;
        return 0;
    }
    *Age = Now - State->Advanced;
    return *Age >= Limit;
}
