// blit_map_cache.h - the CPU mappings of the CPU present blit's source and destination (BD-289, second form).
//
// Until tester.36 the blit mapped its source and its destination with MmMapIoSpaceEx on every present and
// released both at the end of the present. The source is mapped with 4 KiB pages, the destination (the primary,
// 2 MiB aligned in VRAM) with 2 MiB pages, so the system PTE pool got the same VA back again and again, as 4 KiB
// pages in one present and as a 2 MiB page in the next. Two full dumps (tester.35 2026-11-02 08:24Z, published
// tester.36 2026-11-03 22:21Z) show the same state: one blit thread, no other thread in this driver or in
// MmUnmapIoSpace, the source VA 2 MiB aligned, the destination 0xA00000 above it, and only the first 2 MiB of the
// source without PTEs, under a PDE that points to a fresh empty page table. The scan-out mutex of the first fix
// does not touch this path, because nothing else releases a mapping during the copy.
//
// Hypothesis, not yet proved on the lab: the fault comes from the release and reuse of that VA by the mapping calls
// of this path. The rule here takes the churn out of the path in any case: a mapping for a (physical address,
// length, access) is made once and kept, and the next present with the same key gets the same mapping. The working
// set of the CPU path at boot is a few surfaces (the shadow surface and the primary), so the slots below hold it.
// A key that finds no slot takes the least recently used slot: that is the only release a present makes, and it
// happens only when the working set changes (a new mode, more window surfaces than slots). All slots are released
// at stop (dcn.c's DcnUnmapScanout).
//
// The cache has no lock of its own. The caller holds the scan-out mutex (Device->DcnScanoutLock) for every call
// and for as long as it uses a mapping it was given, as for scanout_map.h.
//
// No kernel types here, so that the host test (driver/kmd/test/blit_map_cache_test.c) compiles the same code.
#pragma once
#include <stddef.h>

#define BC250_BLIT_MAP_SLOTS 6

typedef struct BC250_BLIT_MAP_SLOT {
    void* Mapping;                      // NULL: the slot is free
    unsigned long long Physical;
    size_t Length;
    unsigned long Access;               // the access the caller asked for (PAGE_READONLY or PAGE_READWRITE)
    unsigned long long LastUse;         // the value of Clock at the last get; the smallest is evicted first
} BC250_BLIT_MAP_SLOT;

typedef struct BC250_BLIT_MAP_CACHE {
    BC250_BLIT_MAP_SLOT Slots[BC250_BLIT_MAP_SLOTS];
    unsigned long long Clock;
    unsigned long Hits, Maps, Evictions, Failures, Forgotten;   // counters for the log and the escape
} BC250_BLIT_MAP_CACHE;

typedef struct BC250_BLIT_MAP_OPS {
    void* Context;
    // Maps Length bytes at Physical with Access for the CPU; NULL on failure.
    void* (*Map)(void* Context, unsigned long long Physical, size_t Length, unsigned long Access);
    void (*Unmap)(void* Context, void* Mapping, size_t Length);
} BC250_BLIT_MAP_OPS;

// The mapping of the key, made now or kept from an earlier get; NULL when the map call failed (the cache is then
// as it was, except that an evicted slot stays released).
static __inline void* BlitMapGet(BC250_BLIT_MAP_CACHE* Cache, unsigned long long Physical, size_t Length,
                                 unsigned long Access, const BC250_BLIT_MAP_OPS* Ops)
{
    BC250_BLIT_MAP_SLOT* victim = NULL;
    void* map;
    unsigned i;
    if (Length == 0) return NULL;
    Cache->Clock++;
    for (i = 0; i < BC250_BLIT_MAP_SLOTS; i++)
    {
        BC250_BLIT_MAP_SLOT* s = &Cache->Slots[i];
        if (s->Mapping != NULL && s->Physical == Physical && s->Length == Length && s->Access == Access)
        {
            s->LastUse = Cache->Clock;
            Cache->Hits++;
            return s->Mapping;
        }
    }
    // A free slot first; else the least recently used one.
    for (i = 0; i < BC250_BLIT_MAP_SLOTS; i++)
    {
        BC250_BLIT_MAP_SLOT* s = &Cache->Slots[i];
        if (s->Mapping == NULL) { victim = s; break; }
        if (victim == NULL || s->LastUse < victim->LastUse) victim = s;
    }
    if (victim->Mapping != NULL)
    {
        Ops->Unmap(Ops->Context, victim->Mapping, victim->Length);
        Cache->Evictions++;
    }
    victim->Mapping = NULL;
    victim->Physical = 0;
    victim->Length = 0;
    victim->Access = 0;
    victim->LastUse = 0;
    map = Ops->Map(Ops->Context, Physical, Length, Access);
    if (map == NULL) { Cache->Failures++; return NULL; }
    victim->Mapping = map;
    victim->Physical = Physical;
    victim->Length = Length;
    victim->Access = Access;
    victim->LastUse = Cache->Clock;
    Cache->Maps++;
    return map;
}

// Releases every mapping and clears the cache. The caller holds the scan-out mutex.
static __inline void BlitMapRelease(BC250_BLIT_MAP_CACHE* Cache, const BC250_BLIT_MAP_OPS* Ops)
{
    unsigned i;
    for (i = 0; i < BC250_BLIT_MAP_SLOTS; i++)
    {
        BC250_BLIT_MAP_SLOT* s = &Cache->Slots[i];
        if (s->Mapping != NULL) Ops->Unmap(Ops->Context, s->Mapping, s->Length);
        s->Mapping = NULL;
        s->Physical = 0;
        s->Length = 0;
        s->Access = 0;
        s->LastUse = 0;
    }
    Cache->Clock = 0;
}

// Drops the slot of Mapping without an unmap, and leaves that VA mapped for ever. Only for a mapping whose PTEs are
// known to be no longer this driver's (the lab flavour's BD-289 probe found them released or reused): an unmap
// would release PTEs that another mapping now owns. Returns 1 when a slot held Mapping.
static __inline int BlitMapForget(BC250_BLIT_MAP_CACHE* Cache, const void* Mapping)
{
    unsigned i;
    for (i = 0; i < BC250_BLIT_MAP_SLOTS; i++)
    {
        BC250_BLIT_MAP_SLOT* s = &Cache->Slots[i];
        if (Mapping != NULL && s->Mapping == Mapping)
        {
            s->Mapping = NULL;
            s->Physical = 0;
            s->Length = 0;
            s->Access = 0;
            s->LastUse = 0;
            Cache->Forgotten++;
            return 1;
        }
    }
    return 0;
}
