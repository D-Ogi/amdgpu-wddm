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

/* The opener's check of a received BC2A allocation blob, field by field, so that every writer of this form passes
 * (the D3D11 writer above and a D3D12 writer that selects another alignment). *AllocSize is the allocation's size on
 * BC250_SR_WHY_OK. Admitted: magic, version BC250_UMD_ALLOC_VERSION_CACHE_POLICY, the struct size, flags exactly
 * BC250_UMD_A_SHARED, the VRAM heap, gem_flags exactly NO_CPU_ACCESS, va_size equal to alloc_size, alloc_size a page
 * multiple and at least the record's MemorySize, phys_alignment a power of two from the record's MemoryAlignment to
 * BC250_SHARED_RESOURCE_MAX_ALIGNMENT, and 0 in every other byte. An unknown flag or a field that a later version
 * sets is refused with BC250_SR_WHY_ALLOCATION_RECORD.
 * The kernel driver never writes into the allocation private data of this form (driver/kmd/umd_blob.c says so, and
 * driver/contract/test/shared-resource-test.cpp holds it). That rule stays, but it no longer decides an open: this
 * check reads fields, not the bytes of one writer. The opener's own memory fit is Bc250SharedResourceMatchWhy's. */
static __inline const char *Bc250SharedResourceAllocWhy(const void *Data, unsigned int Bytes,
                                                       const BC250_SHARED_RESOURCE *R, unsigned long long *AllocSize)
{
    struct bc250_umd_alloc_private blob;
    const unsigned char *s = (const unsigned char *)Data;
    unsigned char *d = (unsigned char *)&blob;
    unsigned long long bytes, align;
    unsigned int i;
    if (!Data || !R || !AllocSize) return BC250_SR_WHY_ARGUMENTS;
    *AllocSize = 0;
    if (Bytes != sizeof(blob)) return BC250_SR_WHY_ALLOCATION_LENGTH;
    if (!R->MemorySize || R->MemorySize > BC250_SR_MEMORY_MAX || !Bc250SharedResourcePowerOfTwo(R->MemoryAlignment) ||
        R->MemoryAlignment > BC250_SHARED_RESOURCE_MAX_ALIGNMENT) return BC250_SR_WHY_MEMORY;
    for (i = 0; i < sizeof(blob); ++i) d[i] = s[i];
    bytes = blob.alloc_size; align = blob.phys_alignment;
    if (blob.magic != BC250_UMD_ALLOC_MAGIC || blob.version != BC250_UMD_ALLOC_VERSION_CACHE_POLICY ||
        blob.size != sizeof(blob) || blob.flags != BC250_UMD_A_SHARED || blob.preferred_heap != AMDGPU_GEM_DOMAIN_VRAM ||
        blob.gem_flags != AMDGPU_GEM_CREATE_NO_CPU_ACCESS || blob.va_size != bytes || !bytes ||
        (bytes & (BC250_SHARED_RESOURCE_PAGE - 1)) || bytes < R->MemorySize || !Bc250SharedResourcePowerOfTwo(align) ||
        align < R->MemoryAlignment || align > BC250_SHARED_RESOURCE_MAX_ALIGNMENT)
        return BC250_SR_WHY_ALLOCATION_RECORD;
    /* Every other byte is 0: clear the fields checked above and read what is left. */
    blob.magic = blob.version = blob.size = blob.flags = blob.preferred_heap = 0;
    blob.alloc_size = blob.phys_alignment = blob.gem_flags = blob.va_size = 0;
    for (i = 0; i < sizeof(blob); ++i)
        if (d[i]) return BC250_SR_WHY_ALLOCATION_RECORD;
    *AllocSize = bytes;
    return BC250_SR_WHY_OK;
}

#if defined(__cplusplus)
}
#endif

#endif /* BC250_SHARED_RESOURCE_ALLOC_H */
