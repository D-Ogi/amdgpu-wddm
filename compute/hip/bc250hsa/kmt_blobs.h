/* kmt_blobs.h - the three private-data blobs bc250kmd reads, and the offsets of the
 * capability blob this library reads back.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.2. The layout is
 * driver/contract/bc250_umd_submit.h and driver/contract/bc250_umd_private.h. This
 * file keeps its own copy of the field layout, as our Mesa fork keeps
 * radv_wddm2_bc250.h, so that a user-mode C11 component does not pull the imported
 * Linux user API headers into every translation unit.
 *
 * A copy that drifts is a build failure: compute/hip/build.ps1 compares the magic
 * values and the blob sizes against the contract headers, and the static assertions
 * below hold the shapes.
 */
#ifndef BC250HSA_KMT_BLOBS_H
#define BC250HSA_KMT_BLOBS_H

#include <stdint.h>

/* "BC2A", "BC2C", "BC2S", "BC25", little endian. */
#define BC250HSA_ALLOC_MAGIC   0x41324342u
#define BC250HSA_CONTEXT_MAGIC 0x43324342u
#define BC250HSA_SUBMIT_MAGIC  0x53324342u
#define BC250HSA_CAPS_MAGIC    0x35324342u

/* BC2A version 2 makes the cache intent explicit; version 1 always gave write
 * combined backing. gem_flags is then authoritative. */
#define BC250HSA_ALLOC_VERSION   2u
#define BC250HSA_CONTEXT_VERSION 2u
#define BC250HSA_SUBMIT_VERSION  1u

/* AMDGPU_GEM_DOMAIN_GTT and _VRAM. */
#define BC250HSA_HEAP_GTT  0x2u
#define BC250HSA_HEAP_VRAM 0x4u
/* AMDGPU_HW_IP_GFX. On gfx1013 this is the only choice (fact M50). */
#define BC250HSA_IP_GFX 0u
/* AMDGPU_GEM_CREATE_* bits radv_amdgpu_bo.c sets, bit for bit. */
#define BC250HSA_GEM_CPU_ACCESS_REQUIRED (1ull << 0)
#define BC250HSA_GEM_NO_CPU_ACCESS       (1ull << 1)
#define BC250HSA_GEM_CPU_GTT_USWC        (1ull << 2)
/* bc250_umd_alloc_private::flags */
#define BC250HSA_A_EXACT_VA 0x1u

struct bc250hsa_alloc_blob {
    uint32_t magic, version, size, flags;
    uint64_t alloc_size;
    uint64_t phys_alignment;
    uint32_t preferred_heap;
    uint32_t reserved0;
    uint64_t gem_flags;
    uint64_t requested_va;
    uint64_t va_size;
    uint64_t va_flags;
    uint64_t va_offset;
    uint32_t metadata_size;
    uint32_t metadata[16];
    uint32_t reserved[11];
};

struct bc250hsa_context_blob {
    uint32_t magic, version, size, flags;
    uint32_t ip_type, ip_instance, ring, priority, stable_pstate;
    uint32_t reserved[7];
    uint32_t node_ordinal;
    uint32_t reserved_v2[3];
};

struct bc250hsa_ib_blob {
    uint64_t va_start;
    uint32_t ib_bytes;
    uint32_t ip_type;
    uint32_t ip_instance;
    uint32_t ring;
    uint32_t ib_flags;
    uint32_t reserved;
};

#define BC250HSA_SUBMIT_MAX_IBS 16u

struct bc250hsa_submit_blob {
    uint32_t magic, version, size, flags;
    uint32_t ip_type;
    uint32_t num_ibs;
    uint64_t fence_va;
    uint64_t fence_value;
    struct bc250hsa_ib_blob ib[BC250HSA_SUBMIT_MAX_IBS];
    uint32_t reserved[6];
};

/* The kernel driver validates size == offsetof(ib) + num_ibs * sizeof(ib[0]) before
 * it reads anything else, so a submission sends the used prefix and not sizeof. */
#define BC250HSA_SUBMIT_PREFIX_BYTES 40u

#define BC250HSA_ALLOC_BLOB_BYTES   192u
#define BC250HSA_CONTEXT_BLOB_BYTES 80u
#define BC250HSA_SUBMIT_BLOB_BYTES  576u

/* The private data D3DKMTCreateAllocation2 itself carries, beside the per-allocation
 * blob. Shape and checksum are our Mesa fork's (radv_wddm2_bo.c, create_alloc_pdata
 * and calculate_checksum), which ran on unit A. */
struct bc250hsa_create_alloc_pdata {
    uint32_t adapter_id;
    uint32_t _dw1;
    uint32_t flags;      /* 0x80 */
    uint32_t checksum;
    uint32_t reserved[11];
    uint32_t pdata_size;
};

static __inline uint32_t bc250hsa_pdata_checksum(const uint32_t* data, uint32_t dword_count)
{
    uint32_t acc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t i;
    for (i = 0; i < dword_count; i++) {
        acc[i % 8u] += i ^ data[i];
    }
    return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

/* --------------------------------------------------------------------------
 * The capability blob, KMTQAITYPE_UMDRIVERPRIVATE
 * ------------------------------------------------------------------------ */

/* Version 3 of driver/contract/bc250_umd_private.h is 1472 bytes. The offsets below
 * are the ones this library reads; the names are the contract's own. The device half
 * is the kernel's 448-byte struct drm_amdgpu_info_device at blob offset 16, so a
 * field offset is 16 + its offset inside that structure. */
#define BC250HSA_CAPS_BYTES 1472u

#define BC250HSA_CAPS_OFF_MAGIC            0u
#define BC250HSA_CAPS_OFF_VERSION          4u
#define BC250HSA_CAPS_OFF_SIZE             8u
#define BC250HSA_CAPS_OFF_DEVICE           16u
#define BC250HSA_CAPS_OFF_DEVICE_ID        (BC250HSA_CAPS_OFF_DEVICE + 0u)
#define BC250HSA_CAPS_OFF_NUM_SE           (BC250HSA_CAPS_OFF_DEVICE + 20u)
#define BC250HSA_CAPS_OFF_MAX_ENGINE_CLOCK (BC250HSA_CAPS_OFF_DEVICE + 32u)  /* KHz, u64 */
#define BC250HSA_CAPS_OFF_MAX_MEMORY_CLOCK (BC250HSA_CAPS_OFF_DEVICE + 40u)  /* KHz, u64 */
#define BC250HSA_CAPS_OFF_CU_ACTIVE        (BC250HSA_CAPS_OFF_DEVICE + 48u)
#define BC250HSA_CAPS_OFF_VA_OFFSET        (BC250HSA_CAPS_OFF_DEVICE + 144u) /* u64 */
#define BC250HSA_CAPS_OFF_VA_MAX           (BC250HSA_CAPS_OFF_DEVICE + 152u) /* u64 */
#define BC250HSA_CAPS_OFF_VRAM_BIT_WIDTH   (BC250HSA_CAPS_OFF_DEVICE + 180u)
#define BC250HSA_CAPS_OFF_WAVE_FRONT_SIZE  (BC250HSA_CAPS_OFF_DEVICE + 240u)
/* struct drm_amdgpu_memory_info: three 32-byte heaps, each starting with
 * total_heap_size. */
#define BC250HSA_CAPS_OFF_MEMORY           464u
#define BC250HSA_CAPS_OFF_VRAM_TOTAL       (BC250HSA_CAPS_OFF_MEMORY + 0u)
#define BC250HSA_CAPS_OFF_VIS_VRAM_TOTAL   (BC250HSA_CAPS_OFF_MEMORY + 32u)
#define BC250HSA_CAPS_OFF_GTT_TOTAL        (BC250HSA_CAPS_OFF_MEMORY + 64u)
#define BC250HSA_CAPS_OFF_HW_IP            560u
#define BC250HSA_CAPS_OFF_HW_IP_MASK       960u
/* struct bc250_umd_kernel */
#define BC250HSA_CAPS_OFF_DRM_MAJOR        1272u
#define BC250HSA_CAPS_OFF_PCI_DOMAIN       1288u
#define BC250HSA_CAPS_OFF_PCI_BUS          1292u
#define BC250HSA_CAPS_OFF_PCI_DEV          1296u
#define BC250HSA_CAPS_OFF_PCI_FUNC         1300u
#define BC250HSA_CAPS_OFF_SUBMITTABLE_NODES 1408u

/* The GPU address window this library allocates from. It is this device's own and is
 * never shared with the Vulkan driver in the same process, whose own heap starts at
 * 0x0000000200000000 (radv_wddm2_bo.h, RADV_WDDM2_HEAP_START). bc250hsa_open refuses
 * a window that does not lie inside BC250HSA_CAPS_OFF_VA_OFFSET to
 * BC250HSA_CAPS_OFF_VA_MAX, the range the kernel driver states for user mode. */
#define BC250HSA_VA_WINDOW_START 0x0000004000000000ull

#endif /* BC250HSA_KMT_BLOBS_H */
