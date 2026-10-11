// The three private-data blobs a user-mode driver hands the KMD (driver/contract/bc250_umd_submit.h).
//
// This file is the reader. It does not include the contract header and it does not include a WDK header:
// the numbers below are the contract's, and driver/kmd/test/umd_blob_test.c is what keeps them from
// drifting (it includes the real header and checks every constant and offset this reader uses). The KMD
// calls these functions and decides what a refusal means. A refusal here is a shape or a policy this
// milestone will not run, not a guess at a different layout.
//
// M8 policy baked into the reader, because a later caller forgetting it would put a UMD packet on the
// wrong engine: the only IP is GFX (compute on this chip is the gfx ring, fact M50), and the only node
// is node 0. Node 1 is the paging node and has no VMID submit. A second IB is a valid blob and is
// reported as such; this reader does not pretend one IB is the whole submission.
#pragma once

#define UMD_BLOB_ALLOC_MAGIC    0x41324342u   // "BC2A"
#define UMD_BLOB_CONTEXT_MAGIC  0x43324342u   // "BC2C"
#define UMD_BLOB_SUBMIT_MAGIC   0x53324342u   // "BC2S"

#define UMD_BLOB_ALLOC_BYTES    192u          // version 1, the whole struct
#define UMD_BLOB_ALLOC_VERSION_SCANOUT 3u     // the version that put scanout_* in the reserved tail
#define UMD_BLOB_ALLOC_SCANOUT_AT 148u        // offset of scanout_width; the four words follow it
#define UMD_BLOB_CONTEXT_V1     64u           // version 1 ended here; node_ordinal starts at this offset
#define UMD_BLOB_CONTEXT_BYTES  80u           // version 2
#define UMD_BLOB_SUBMIT_BYTES   576u          // the whole struct, reserved tail included: what CreateContext
                                              // must offer as DmaBufferPrivateDataSize
#define UMD_BLOB_SUBMIT_PREFIX  40u           // bytes before ib[0]
#define UMD_BLOB_IB_BYTES       32u
#define UMD_BLOB_SUBMIT_MAX_IBS 16u

#define UMD_BLOB_HEAP_GTT       0x2u          // AMDGPU_GEM_DOMAIN_GTT
#define UMD_BLOB_HEAP_VRAM      0x4u          // AMDGPU_GEM_DOMAIN_VRAM
#define UMD_BLOB_GEM_NO_CPU_ACCESS 0x2ull // AMDGPU_GEM_CREATE_NO_CPU_ACCESS
#define UMD_BLOB_GEM_GTT_USWC      0x4ull // AMDGPU_GEM_CREATE_CPU_GTT_USWC
#define UMD_BLOB_GEM_DISCARDABLE   0x1000ull // AMDGPU_GEM_CREATE_DISCARDABLE

#define UMD_BLOB_IP_GFX         0u
#define UMD_BLOB_IP_COMPUTE     1u
#define UMD_BLOB_IP_DMA         2u
#define UMD_BLOB_NODE_3D        0u

#define UMD_BLOB_A_EXACT_VA     0x00000001u
#define UMD_BLOB_A_SPARSE       0x00000002u
#define UMD_BLOB_A_USERPTR      0x00000004u
#define UMD_BLOB_A_SCANOUT      0x00000010u

// alloc_size above this does not round up to a page inside 64 bits. Larger is refused, not truncated.
#define UMD_BLOB_ALLOC_MAX      0xFFFFFFFFFFFFF000ull

#define UMD_BLOB_OK             0
#define UMD_BLOB_TOO_SMALL      1
#define UMD_BLOB_BAD_MAGIC      2
#define UMD_BLOB_BAD_VERSION    3
#define UMD_BLOB_BAD_SIZE       4
#define UMD_BLOB_BAD_HEAP       5
#define UMD_BLOB_BAD_FLAGS      6
#define UMD_BLOB_BAD_VA         7
#define UMD_BLOB_BAD_IP         8
#define UMD_BLOB_BAD_NODE       9
#define UMD_BLOB_BAD_IB         10
#define UMD_BLOB_BAD_SCANOUT    11

// A short fixed string for the guard log. Never NULL.
const char* UmdBlobStatusText(int status);

// TRUE when the first word is the allocation magic. A short or empty buffer is FALSE, not a crash.
int UmdBlobIsAlloc(const void* bytes, unsigned len);

// The first little-endian word, or 0 when there are not four bytes. For a refusal log, not a parse.
unsigned long UmdBlobFirstWord(const void* bytes, unsigned len);

struct umd_alloc_view {
    unsigned long long bytes;
    unsigned long long alignment;     // phys_alignment as the winsys wrote it; 0 means it wrote none
    unsigned long long requested_va;  // recorded. This reader does not place it: VidMm does, later.
    unsigned long heap;               // UMD_BLOB_HEAP_GTT or UMD_BLOB_HEAP_VRAM
    unsigned long flags;
    unsigned long version;            // the blob's own version word, recorded for the paging journal (KMD193)
    unsigned long long gem_flags;    // full BC2A cache/access intent, no truncation
    int cache_policy_valid;           // BC2A v2+, v1 retains legacy WC behavior
    int exact_va;
    // BC2A v3 and UMD_BLOB_A_SCANOUT: the surface the display pipeline would read. This reader
    // checks the shape only - version, VRAM heap, nonzero geometry, a pitch that covers the rows.
    // Whether such a surface may reach HUBP0 is scanout_admit.h's decision, against the POST mode
    // the firmware left and the segment VidMm placed the allocation in.
    int scanout;
    unsigned long scanout_width, scanout_height, scanout_pitch, scanout_format;
};

struct umd_context_view {
    unsigned long ip_type;
    unsigned long node_ordinal;
    unsigned long priority;
};

// ib_va / ib_bytes are ib[0]. single_ib is 1 only when num_ibs is 1: the one shape the gfx ring
// submit runs. Two IBs still return UMD_BLOB_OK with single_ib 0, so the caller can say "not run"
// instead of "malformed".
struct umd_submit_view {
    unsigned num_ibs;
    unsigned long long ib_va;
    unsigned long ib_bytes;
    int single_ib;
    unsigned long long fence_va, fence_value; /* existing BC2S prefix at 24/32, diagnostic join only */
};

// Cached backing store is requested only for CPU-accessible GTT without USWC.
// VRAM and command-buffer USWC allocations retain write-combined backing store.
int UmdBlobAllocCpuCached(const struct umd_alloc_view* allocation);

// On any refusal *out is zeroed when out is not NULL. bytes may be NULL.
int UmdBlobParseAlloc(const void* bytes, unsigned len, struct umd_alloc_view* out);

// Memory manager stage 1c (0.7.216.8): the segments an allocation may be resident in, for
// DXGK_ALLOCATIONINFO.PreferredSegment and the two supported sets. Segment ids are the KMD's
// one-based ones (vram, aperture); a set has bit (id - 1) for each id, as the DDI defines it.
//   GTT heap            {aperture}, as before: the winsys asked for host memory.
//   VRAM heap, shared   {vram, aperture} in that order: VidMm may demote the allocation to system
//                       memory instead of evicting it. amdgpu does the same for every VRAM-only
//                       buffer that is not a kernel or DISCARDABLE one (amdgpu_object.c
//                       amdgpu_bo_create: allowed_domains |= AMDGPU_GEM_DOMAIN_GTT).
//   VRAM heap, scan-out {vram}: the display core reads only the local segment.
//   VRAM, DISCARDABLE   {vram}: amdgpu gives such a buffer no GTT fallback either.
//   shared == 0         {vram} for every VRAM allocation: the placement before 0.7.216.8.
// Returns 1 when the aperture is the second choice, else 0. A NULL view gets {vram} with no
// second choice; *out is always written.
struct umd_placement {
    unsigned long preferred[2];   // preference order; preferred[1] == 0 means no second choice
    unsigned long supported;      // the read and the write segment set, which are the same here
};
int UmdBlobPlacement(const struct umd_alloc_view* allocation, unsigned long vram, unsigned long aperture,
                     int shared, struct umd_placement* out);

// ddi_node is the node DxgkDdiCreateContext was asked for. The blob's node_ordinal must name that
// same node, and that node must be UMD_BLOB_NODE_3D.
int UmdBlobParseContext(const void* bytes, unsigned len, unsigned ddi_node, struct umd_context_view* out);

// len is the UMD's own byte count, not the KMD buffer it was copied into. The blob's size word must
// equal UMD_BLOB_SUBMIT_PREFIX + num_ibs * UMD_BLOB_IB_BYTES, and len must cover it.
int UmdBlobParseSubmit(const void* bytes, unsigned len, struct umd_submit_view* out);
