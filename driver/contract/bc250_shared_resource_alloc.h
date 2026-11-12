/* SPDX-License-Identifier: MIT */
/*
 * driver/contract/bc250_shared_resource_alloc.h - the BC2A allocation blob of a shared resource in the ENGINE LAYOUT
 * form (bc250_shared_resource.h): its one writer and the opener's check of it. A header of its own because it needs
 * the amdgpu uapi headers of bc250_umd_submit.h (driver/contract/third_party, driver/contract/uapi-shim).
 */
#ifndef BC250_SHARED_RESOURCE_ALLOC_H
#define BC250_SHARED_RESOURCE_ALLOC_H

#include "bc250_shared_resource.h"
#include "bc250_umd_submit.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* The bytes of the BC2A allocation of a resource of this form; the one writer of it. 1 on success, 0 when the
 * record's memory cannot be described (*Blob is then zero). The allocation is the record's memory size rounded
 * up to the larger of its alignment and a page; VidMm selects the address. */
static __inline int Bc250SharedResourceAllocInit(struct bc250_umd_alloc_private *Blob, const BC250_SHARED_RESOURCE *R)
{
    unsigned char *p = (unsigned char *)Blob;
    unsigned long long align, bytes;
    unsigned int i;
    if (!Blob) return 0;
    for (i = 0; i < sizeof(*Blob); ++i) p[i] = 0;
    if (!R || !R->MemorySize || R->MemorySize > BC250_SR_MEMORY_MAX ||
        !Bc250SharedResourcePowerOfTwo(R->MemoryAlignment) || R->MemoryAlignment > BC250_SHARED_RESOURCE_MAX_ALIGNMENT)
        return 0;
    align = R->MemoryAlignment > BC250_SHARED_RESOURCE_PAGE ? R->MemoryAlignment : BC250_SHARED_RESOURCE_PAGE;
    bytes = (R->MemorySize + align - 1) & ~(align - 1);
    Blob->magic = BC250_UMD_ALLOC_MAGIC;
    Blob->version = BC250_UMD_ALLOC_VERSION_CACHE_POLICY;
    Blob->size = (__u32)sizeof(*Blob);
    Blob->flags = BC250_UMD_A_SHARED;
    Blob->alloc_size = bytes;
    Blob->phys_alignment = align;
    Blob->preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
    Blob->gem_flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
    Blob->va_size = bytes;
    return 1;
}

/* The opener's check of a received BC2A allocation blob: exactly the bytes Bc250SharedResourceAllocInit writes for
 * the record. *AllocSize is the allocation's size on BC250_SR_WHY_OK. */
static __inline const char *Bc250SharedResourceAllocWhy(const void *Data, unsigned int Bytes,
                                                       const BC250_SHARED_RESOURCE *R, unsigned long long *AllocSize)
{
    struct bc250_umd_alloc_private expected;
    const unsigned char *a = (const unsigned char *)Data, *b = (const unsigned char *)&expected;
    unsigned int i;
    if (!Data || !R || !AllocSize) return BC250_SR_WHY_ARGUMENTS;
    *AllocSize = 0;
    if (Bytes != sizeof(expected)) return BC250_SR_WHY_ALLOCATION_LENGTH;
    if (!Bc250SharedResourceAllocInit(&expected, R)) return BC250_SR_WHY_MEMORY;
    for (i = 0; i < sizeof(expected); ++i)
        if (a[i] != b[i]) return BC250_SR_WHY_ALLOCATION_RECORD;
    *AllocSize = expected.alloc_size;
    return BC250_SR_WHY_OK;
}

#if defined(__cplusplus)
}
#endif

#endif /* BC250_SHARED_RESOURCE_ALLOC_H */
