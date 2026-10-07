/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* guard_keep_prune.h - which of the kept log ring files under C:\BC250\kmdlog may stay (guard.c GuardLogKeep).
 *
 * KeepLog writes the ring into a file, and until 0.7.216.10 every call of GuardLogKeep made a new one. A start with
 * the tracing gates open calls it about fifteen times (startup.c at each stage, gfx.c at each CP step and RLC
 * boundary, gpumem.c at the bootstrap TLB), so one start left fifteen files holding the same early lines, and
 * nothing ever removed one: unit A had 11993 files and 134 MB after a fortnight. The driver now writes one file per
 * start and one per stop, appending each checkpoint to the file of its episode, and keeps at most the newest
 * BC250_KEEP_FILES of them.
 *
 * The selection is here, as pure integer and character work, because the kernel half of it (ZwQueryDirectoryFile,
 * ZwDeleteFile in a PnP start) is the half a host test cannot drive. Offer every name in the directory, in any
 * order, and the table holds the BC250_KEEP_FILES largest. Bc250KeepPruneLimit then names the oldest file that may
 * stay, and Bc250KeepPruneGoes answers for each name whether it is older than that - which is the second pass over
 * the directory.
 *
 * Ordinal comparison of the name is comparison by time, because the name is
 * ring-<YYYYMMDD>-<HHMMSS>-<mmm>[-<label>].log: a constant prefix, then the UTC time in fixed-width fields, and
 * only then anything of variable length. Two files of the same millisecond are ordered by their label, which is
 * arbitrary but stable, and one of them is kept over the other. The files of a driver before 0.7.216.10 carry no
 * label and sort by the same rule.
 *
 * Nothing here allocates, and the table is 25 KB: guard.c keeps it in paged pool for the length of one prune, never
 * on the stack. PASSIVE_LEVEL in the caller, for the file work; this header itself needs no IRQL at all.
 */
#ifndef BC250_GUARD_KEEP_PRUNE_H
#define BC250_GUARD_KEEP_PRUNE_H

#define BC250_KEEP_FILES 200u           /* kept ring files; older ones go */
#define BC250_KEEP_NAME 64u             /* wide characters per name, the terminator included */
#define BC250_KEEP_PREFIX L"ring-"
#define BC250_KEEP_SUFFIX L".log"
#define BC250_KEEP_PATTERN L"ring-*.log"        /* the same two, as the file system's search pattern */

typedef struct _BC250_KEEP_PRUNE {
    wchar_t Newest[BC250_KEEP_FILES][BC250_KEEP_NAME];  /* ascending, so [0] is the oldest kept name */
    unsigned Count;                     /* names the table holds */
    unsigned Offered;                   /* ring files offered to it */
    unsigned Ignored;                   /* names that are not ring files of ours, or are too long */
} BC250_KEEP_PRUNE;

/* Ordinal, case sensitive, on the characters a file system gives us. No CRT: this header is included by the driver
 * and by its host test, and the driver links none. */
static __inline int Bc250KeepCompare(const wchar_t* a, const wchar_t* b)
{
    while (*a != 0 && *a == *b) { ++a; ++b; }
    return (*a < *b) ? -1 : ((*a > *b) ? 1 : 0);
}

static __inline int Bc250KeepStarts(const wchar_t* name, const wchar_t* prefix)
{
    while (*prefix != 0) { if (*name != *prefix) return 0; ++name; ++prefix; }
    return 1;
}

/* A kept ring file of ours: ring-....log, short enough to hold. Length is in characters, without a terminator. */
static __inline int Bc250KeepIsRing(const wchar_t* name, unsigned length)
{
    static const wchar_t suffix[] = BC250_KEEP_SUFFIX;
    unsigned tail = (unsigned)(sizeof(suffix) / sizeof(suffix[0]) - 1u);
    unsigned i;

    if (length == 0 || length >= BC250_KEEP_NAME) return 0;
    if (length <= tail) return 0;
    if (!Bc250KeepStarts(name, BC250_KEEP_PREFIX)) return 0;
    for (i = 0; i < tail; ++i) if (name[length - tail + i] != suffix[i]) return 0;
    return 1;
}

static __inline void Bc250KeepPruneBegin(BC250_KEEP_PRUNE* table)
{
    table->Count = 0;
    table->Offered = 0;
    table->Ignored = 0;
    table->Newest[0][0] = 0;
}

/* One directory entry. Returns 1 when it was counted as a ring file of ours, 0 when it was ignored. The name need
 * not be terminated: Length says how long it is. */
static __inline int Bc250KeepPruneOffer(BC250_KEEP_PRUNE* table, const wchar_t* name, unsigned length)
{
    wchar_t held[BC250_KEEP_NAME];
    unsigned i, at;

    if (!Bc250KeepIsRing(name, length)) { table->Ignored++; return 0; }
    for (i = 0; i < length; ++i) held[i] = name[i];
    held[length] = 0;
    table->Offered++;

    /* The table is full and this name is not newer than the oldest it holds: nothing to do. */
    if (table->Count == BC250_KEEP_FILES && Bc250KeepCompare(held, table->Newest[0]) <= 0) return 1;
    if (table->Count == BC250_KEEP_FILES) {
        /* Drop the oldest, shift the rest down, and the array has one free slot at the end. */
        for (i = 0; i + 1 < BC250_KEEP_FILES; ++i)
            for (at = 0; at < BC250_KEEP_NAME; ++at) table->Newest[i][at] = table->Newest[i + 1][at];
        table->Count--;
    }
    /* Insertion sort, ascending. The common case on a file system that enumerates by name is the end. */
    at = table->Count;
    while (at > 0 && Bc250KeepCompare(table->Newest[at - 1], held) > 0) {
        for (i = 0; i < BC250_KEEP_NAME; ++i) table->Newest[at][i] = table->Newest[at - 1][i];
        at--;
    }
    for (i = 0; i < BC250_KEEP_NAME; ++i) table->Newest[at][i] = held[i];
    table->Count++;
    return 1;
}

/* The oldest name that may stay, or NULL when every offered file may stay. */
static __inline const wchar_t* Bc250KeepPruneLimit(const BC250_KEEP_PRUNE* table)
{
    if (table->Offered <= BC250_KEEP_FILES) return 0;
    return table->Newest[0];
}

/* How many files the second pass has to remove. */
static __inline unsigned Bc250KeepPruneGoing(const BC250_KEEP_PRUNE* table)
{
    return (table->Offered > BC250_KEEP_FILES) ? table->Offered - BC250_KEEP_FILES : 0u;
}

/* Does this directory entry go? Only a ring file of ours, and only one older than the limit. */
static __inline int Bc250KeepPruneGoes(const BC250_KEEP_PRUNE* table, const wchar_t* name, unsigned length)
{
    wchar_t held[BC250_KEEP_NAME];
    const wchar_t* limit = Bc250KeepPruneLimit(table);
    unsigned i;

    if (limit == 0) return 0;
    if (!Bc250KeepIsRing(name, length)) return 0;
    for (i = 0; i < length; ++i) held[i] = name[i];
    held[length] = 0;
    return Bc250KeepCompare(held, limit) < 0;
}

#endif /* BC250_GUARD_KEEP_PRUNE_H */
