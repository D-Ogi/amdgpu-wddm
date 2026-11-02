// scanout_map.h - the CPU mapping of the flip target that the CPU present blit copies into (BD-289).
//
// One state (the mapping, the physical address it maps, its length) and one rule, shared by dcn.c's
// DcnScanoutMapping and the host test driver/kmd/test/scanout_lock_test.c. The rule: a request for the mapped
// target and length gives the mapping as it is; any other request releases the old mapping first, then maps the
// new target if it is allowed.
//
// The state has no lock of its own. The caller holds the scan-out mutex (Device->DcnScanoutLock) for every call
// AND for as long as it uses the mapping it was given: a mapping given to one blit is released by the next request
// for another target, so two blits without the mutex can release one mapping twice. The second MmUnmapIoSpace
// then releases page table entries that the system has meanwhile given to a new mapping, such as the source of
// the other blit (the 0x0A of 2026-11-02 08:24Z, all PTEs of the blit source zero at the copy).
//
// No kernel types here, so that the host test compiles the same function.
#pragma once
#include <stddef.h>

typedef struct BC250_SCANOUT_MAP_OPS {
    void* Context;
    // Maps Length bytes at Physical for the CPU; NULL on failure.
    void* (*Map)(void* Context, unsigned long long Physical, size_t Length);
    void (*Unmap)(void* Context, void* Mapping, size_t Length);
    // Nonzero when Physical may be mapped (dcn.c: AddressAllowed, checked again at the point of mapping).
    int (*Allowed)(void* Context, unsigned long long Physical);
    // Optional observers (the lab flavour's BD-289 probe): before the old mapping is released, and after the new
    // mapping call returned (NULL when it failed). They change nothing.
    void (*BeforeRemap)(void* Context, void* OldMapping, unsigned long long OldPhysical, size_t OldLength,
                        unsigned long long NewPhysical, size_t NewLength);
    void (*AfterUnmap)(void* Context);
    void (*AfterMap)(void* Context, void* NewMapping);
} BC250_SCANOUT_MAP_OPS;

#define BC250_SCANOUT_MAP_CACHED  1     // the mapping already maps this target and length
#define BC250_SCANOUT_MAP_NEW     2     // the old mapping (if any) was released and the target mapped now
#define BC250_SCANOUT_MAP_REFUSED 3     // the old mapping was released; the target is not allowed
#define BC250_SCANOUT_MAP_FAILED  4     // the old mapping was released; mapping the target failed

static __inline int ScanoutMapSelect(void** Mapping, unsigned long long* Physical, size_t* Length,
                                     unsigned long long Target, size_t Bytes, const BC250_SCANOUT_MAP_OPS* Ops)
{
    void* map;
    if (*Mapping != NULL && *Physical == Target && *Length == Bytes) return BC250_SCANOUT_MAP_CACHED;
    if (Ops->BeforeRemap) Ops->BeforeRemap(Ops->Context, *Mapping, *Physical, *Length, Target, Bytes);
    if (*Mapping != NULL) Ops->Unmap(Ops->Context, *Mapping, *Length);
    *Mapping = NULL;
    *Physical = 0;
    *Length = 0;
    if (Ops->AfterUnmap) Ops->AfterUnmap(Ops->Context);
    if (!Ops->Allowed(Ops->Context, Target)) return BC250_SCANOUT_MAP_REFUSED;
    map = Ops->Map(Ops->Context, Target, Bytes);
    if (Ops->AfterMap) Ops->AfterMap(Ops->Context, map);
    if (map == NULL) return BC250_SCANOUT_MAP_FAILED;
    *Mapping = map;
    *Physical = Target;
    *Length = Bytes;
    return BC250_SCANOUT_MAP_NEW;
}

// Releases the mapping, if any, and clears the state. The caller holds the scan-out mutex.
static __inline void ScanoutMapRelease(void** Mapping, unsigned long long* Physical, size_t* Length,
                                       const BC250_SCANOUT_MAP_OPS* Ops)
{
    if (*Mapping != NULL) Ops->Unmap(Ops->Context, *Mapping, *Length);
    *Mapping = NULL;
    *Physical = 0;
    *Length = 0;
}
