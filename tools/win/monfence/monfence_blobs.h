// monfence_blobs.h - the three private-data blobs monfence hands the KMD, built in one place so that
// monfence_host_test.c can run the exact bytes through the KMD's own reader (driver/kmd/umd_blob.c) on the
// development PC. Shapes are those of experiments/E27-m9-inference (residency-probe.c, gpu-residency-probe.c),
// which ran on unit A under this KMD.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4201)   // imported Linux UAPI anonymous unions
#endif
#include "../../../driver/contract/bc250_umd_submit.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// BC2A version 1: legacy cache intent, so the KMD gives write-combined backing (umd_blob.c, UmdBlobAllocCpuCached
// is false below version 2) and the GPU mapping is not snooped. That is the mapping the E27 readback validated on
// unit A, and the CPU never caches it, so a CPU read sees what the GPU wrote to memory.
static inline void mf_build_alloc_blob(struct bc250_umd_alloc_private* b, uint64_t bytes, uint32_t heap)
{
    memset(b, 0, sizeof(*b));
    b->magic = BC250_UMD_ALLOC_MAGIC;
    b->version = BC250_UMD_ALLOC_VERSION;
    b->size = (uint32_t)sizeof(*b);
    b->alloc_size = bytes;
    b->phys_alignment = 4096;
    b->preferred_heap = heap;
    b->va_size = bytes;
}

// BC2C version 2, GFX, node 0: the only shape the KMD accepts (umd_blob.c, UmdBlobParseContext).
static inline void mf_build_context_blob(struct bc250_umd_context_private* b)
{
    memset(b, 0, sizeof(*b));
    b->magic = BC250_UMD_CONTEXT_MAGIC;
    b->version = BC250_UMD_CONTEXT_VERSION;
    b->size = (uint32_t)sizeof(*b);
    b->ip_type = AMDGPU_HW_IP_GFX;
    b->node_ordinal = 0;
}

// BC2S version 1 with one IB. size is the used prefix (40 + 32), not sizeof: the KMD wants equality. fence_va and
// fence_value are recorded for the log only; no KMD code reads them (grep driver/kmd: only the host test sets them).
// Returns the byte count to pass as PrivateDriverDataSize.
static inline uint32_t mf_build_submit_blob(struct bc250_umd_submit_private* b, uint64_t ib_va, uint32_t ib_bytes,
                                            uint64_t fence_va, uint64_t fence_value)
{
    memset(b, 0, sizeof(*b));
    b->magic = BC250_UMD_SUBMIT_MAGIC;
    b->version = BC250_UMD_SUBMIT_VERSION;
    b->size = (uint32_t)(offsetof(struct bc250_umd_submit_private, ib) + sizeof(b->ib[0]));
    b->ip_type = AMDGPU_HW_IP_GFX;
    b->num_ibs = 1;
    b->fence_va = fence_va;
    b->fence_value = fence_value;
    b->ib[0].va_start = ib_va;
    b->ib[0].ib_bytes = ib_bytes;
    b->ib[0].ip_type = AMDGPU_HW_IP_GFX;
    return b->size;
}
