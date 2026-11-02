// BD-028: table memory budget computation (stage A of the elastic-pool design).
// Pure helper, no side effects or kernel dependencies. The actual layout in
// wddm.c still uses the old 1/32 policy until stages B-D pass.
#ifndef BC250_TABLE_BUDGET_H
#define BC250_TABLE_BUDGET_H

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define BC250_TABLE_ENTRIES_PER_PAGE 512
#define BC250_TABLE_LEVELS 4
#define BC250_TABLE_GIANT_PAGE ((unsigned long long)1 << 30)
#define BC250_TABLE_ALIGNMENT 65536
#define BC250_TABLE_BUDGET_CAP (64ull * 1024 * 1024)

typedef struct _BC250_TABLE_BUDGET {
    unsigned long long ApplicationBytes;
    unsigned long long OrdinaryTableBytes;
    unsigned long long PinnedPagingBytes;
    unsigned long long TotalTableBytes;
    unsigned long long WastedBytes;
} BC250_TABLE_BUDGET;

// Count page-table pages for one process with a contiguous VA mapping.
// VA from BaseVa to BaseVa + Bytes, all at 4K granularity.
// Returns the total page-table pages across all 4 levels.
static __inline unsigned Bc250TablePagesForRange(
    unsigned long long BaseVa, unsigned long long Bytes)
{
    unsigned pages = 0;
    unsigned long long endVa;
    int level;
    if (!Bytes || BaseVa > ~(unsigned long long)0 - Bytes) return 0;
    endVa = BaseVa + Bytes;
    for (level = 0; level < BC250_TABLE_LEVELS; level++) {
        unsigned shift = 12 + 9 * (level + 1);
        unsigned long long firstPrefix = BaseVa >> shift;
        unsigned long long lastPrefix = (endVa - 1) >> shift;
        unsigned long long count = lastPrefix - firstPrefix + 1;
        if (count > 0xFFFFFFFFu) count = 0xFFFFFFFFu;
        pages += (unsigned)count;
    }
    return pages;
}

// The pinned paging process uses a 1 GiB VA window.
// Its hierarchy: 512 L0 + 1 L1 + 1 L2 + 1 L3 root = 515 pages.
static __inline unsigned Bc250PinnedPagingPages(void)
{
    return Bc250TablePagesForRange(0, BC250_TABLE_GIANT_PAGE);
}

// Current policy (1/32, min 4 MiB): a single shared segment for both
// ordinary and pinned paging tables. This is the control.
static __inline int Bc250TableBudgetCurrent(
    unsigned long long Available, BC250_TABLE_BUDGET* Out)
{
    unsigned long long tableBytes;
    if (!Out || Available < BC250_TABLE_ALIGNMENT) return 0;
    tableBytes = (Available / 32u) & ~(BC250_TABLE_ALIGNMENT - 1);
    if (tableBytes < 4ull * 1024 * 1024) tableBytes = 4ull * 1024 * 1024;
    if (tableBytes >= Available || Available - tableBytes < BC250_TABLE_ALIGNMENT) return 0;
    Out->PinnedPagingBytes = 0;
    Out->OrdinaryTableBytes = 0;
    Out->TotalTableBytes = tableBytes;
    Out->ApplicationBytes = Available - tableBytes;
    Out->WastedBytes = 0;
    return 1;
}

// Proposed policy (stage A model): pinned paging gets a dedicated reservation
// rounded to 64K; the rest of the table reserve returns to application local.
// No ordinary table reserve: ordinary tables would share the application
// segment under VidMm allocation in stages B-D. Until then this just shows
// the split, not a deployable layout.
static __inline int Bc250TableBudgetProposed(
    unsigned long long Available, BC250_TABLE_BUDGET* Out)
{
    unsigned long long pinnedBytes, current;
    unsigned pinnedPages;
    if (!Out || Available < BC250_TABLE_ALIGNMENT) return 0;
    pinnedPages = Bc250PinnedPagingPages();
    pinnedBytes = (unsigned long long)pinnedPages * PAGE_SIZE;
    pinnedBytes = (pinnedBytes + BC250_TABLE_ALIGNMENT - 1) & ~(BC250_TABLE_ALIGNMENT - 1);
    // Current total for comparison
    current = (Available / 32u) & ~(BC250_TABLE_ALIGNMENT - 1);
    if (current < 4ull * 1024 * 1024) current = 4ull * 1024 * 1024;
    if (current >= Available) return 0;
    if (pinnedBytes >= Available || Available - pinnedBytes < BC250_TABLE_ALIGNMENT) return 0;
    Out->PinnedPagingBytes = pinnedBytes;
    Out->OrdinaryTableBytes = 0;
    Out->TotalTableBytes = pinnedBytes;
    Out->ApplicationBytes = Available - pinnedBytes;
    Out->WastedBytes = current > pinnedBytes ? current - pinnedBytes : 0;
    return 1;
}

#endif
