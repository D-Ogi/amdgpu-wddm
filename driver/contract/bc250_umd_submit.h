/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/bc250_umd_submit.h - the private-data blobs for allocation, context and
 * submission.
 *
 * bc250_umd_private.h answers "what is this GPU". This header answers "do this with it". Three
 * blobs, each riding in a `pPrivateDriverData` field that dxgkrnl passes through without looking:
 *
 *   BC250_UMD_ALLOC_PRIVATE     D3DKMTCreateAllocation2 -> DxgkDdiCreateAllocation
 *   BC250_UMD_CONTEXT_PRIVATE   D3DKMTCreateContextVirtual -> DxgkDdiCreateContext
 *   BC250_UMD_SUBMIT_PRIVATE    D3DKMTSubmitCommand -> DxgkDdiSubmitCommandVirtual
 *
 * WHY THE FIELDS LOOK LIKE amdgpu's
 *
 * Every field below exists because a specific call in RADV's amdgpu winsys sets it. The citations
 * are to <BC250_ROOT>\ref\mesa\src\amd\vulkan\winsys\amdgpu. Keeping the amdgpu spelling is
 * deliberate: a winsys that ports radv_amdgpu_* to WDDM should be filling in fields it recognises,
 * and a disagreement with a Linux capture should be a diff rather than an argument. Where WDDM has
 * no equivalent the field is still here, marked, so the gap is visible instead of forgotten.
 *
 * The KMD reads these in driver/kmd/umd_blob.c (host-tested; node 0 only). The winsys that writes
 * them is driver/icd/mesa-wddm2-bc250.patch. README-winsys.md is the argument.
 * See README-winsys.md "Open questions" before relying on any of it.
 *
 * VERSIONING - the same rule as the caps blob, for the same reason. `magic`, `version` and `size`
 * are the first three words of every blob and never move. Fields are only ever appended and the
 * version is bumped; nothing is removed, nothing is renumbered. A reader that does not recognise
 * the magic must refuse the blob rather than guess, and a reader that sees a version above its
 * own may proceed, because everything it knows is still where it expects.
 */
#ifndef BC250_UMD_SUBMIT_H
#define BC250_UMD_SUBMIT_H

/* For __u32/__u64 and the AMDGPU_* constants the comments cite. Same import as the caps blob;
 * see third_party/PROVENANCE.md. */
#include "amdgpu_drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* "BC2A" / "BC2C" / "BC2S" little endian. Distinct per blob so a blob delivered to the wrong DDI
 * is rejected by inspection rather than silently misparsed - the three arrive through three
 * different entry points and nothing in dxgkrnl would catch a swap. */
#define BC250_UMD_ALLOC_MAGIC       0x41324342u
#define BC250_UMD_CONTEXT_MAGIC     0x43324342u
#define BC250_UMD_SUBMIT_MAGIC      0x53324342u

#define BC250_UMD_ALLOC_VERSION     1u // legacy producer, cache intent unspecified
#define BC250_UMD_ALLOC_VERSION_CACHE_POLICY 2u // same wire size; gem_flags is authoritative
/* 3: same wire size again. The four scanout_* words below are read, and only then, so a version 2
 * producer keeps its zeroed reserved tail and its old meaning. M15.14. */
#define BC250_UMD_ALLOC_VERSION_SCANOUT 3u
/* 2: version 1 plus node_ordinal, appended - ADR 0013's node layout, see the struct below. */
#define BC250_UMD_CONTEXT_VERSION   2u
#define BC250_UMD_SUBMIT_VERSION    1u

/* ---------------------------------------------------------------------------------------------
 * 1. Allocation.  D3DKMTCreateAllocation2 -> DxgkDdiCreateAllocation
 *
 * The amdgpu original is one GEM_CREATE ioctl: radv_amdgpu_bo.c:633 ac_drm_bo_alloc() with a
 * struct drm_amdgpu_gem_create_in {alloc_size, phys_alignment, preferred_heap, flags}. WDDM
 * splits that into an allocation (this blob) and a separate VA mapping (see section 2), so the
 * VA-side fields are carried here as a request that DxgkDdiCreateAllocation records and
 * DxgkDdiBuildPagingBuffer later honours.
 * ------------------------------------------------------------------------------------------- */

/* bc250_umd_alloc_private::flags */
#define BC250_UMD_A_EXACT_VA        0x00000001u  /* requested_va must be honoured exactly, or the
                                                  * allocation fails. See README-winsys.md: RADV
                                                  * assigns VAs itself from util_vma_heap and
                                                  * bakes them into descriptors before any
                                                  * mapping happens, so "VidMm picked a different
                                                  * address" is not a recoverable outcome. */
#define BC250_UMD_A_SPARSE          0x00000002u  /* PRT. No backing store; the mapping is
                                                  * AMDGPU_VM_PAGE_PRT (radv_amdgpu_bo.c:173). */
#define BC250_UMD_A_USERPTR         0x00000004u  /* backed by existing user pages: the
                                                  * ac_drm_create_bo_from_user_mem() path
                                                  * (radv_amdgpu_bo.c:819). OPEN QUESTION. */
#define BC250_UMD_A_SHARED          0x00000008u  /* exported or imported across processes
                                                  * (ac_drm_bo_export/import). */
#define BC250_UMD_A_SCANOUT         0x00000010u  /* this allocation is meant to be the argument of
                                                  * SetVidPnSourceAddress: the display pipeline, not
                                                  * the compositor, reads it. It has no amdgpu
                                                  * original - on Linux the scanout surface is the
                                                  * KMS plane's, described by a framebuffer object -
                                                  * and it exists because WDDM hands the kernel
                                                  * driver an allocation handle and nothing else at
                                                  * flip time. The four scanout_* words must then
                                                  * describe the surface, the version must be at
                                                  * least BC250_UMD_ALLOC_VERSION_SCANOUT and the
                                                  * heap must be AMDGPU_GEM_DOMAIN_VRAM: the
                                                  * aperture is not a scanned-out segment. The flag
                                                  * is a request, never a permission - the kernel
                                                  * driver re-derives every one of these facts and
                                                  * refuses the flip otherwise (M15.14). */

struct bc250_umd_alloc_private {
    __u32 magic;                        /* BC250_UMD_ALLOC_MAGIC */
    __u32 version;                      /* BC250_UMD_ALLOC_VERSION */
    __u32 size;                         /* sizeof(struct bc250_umd_alloc_private) */
    __u32 flags;                        /* BC250_UMD_A_* */

    /* --- struct drm_amdgpu_gem_create_in, field for field ------------------------------------
     * radv_amdgpu_bo.c:633. The heap and flag values are amdgpu's own; see the AMDGPU_GEM_*
     * constants in amdgpu_drm.h rather than inventing a parallel numbering. */
    __u64 alloc_size;                   /* bytes, already aligned by the winsys */
    __u64 phys_alignment;               /* radv passes the allocation's alignment here */
    __u32 preferred_heap;               /* AMDGPU_GEM_DOMAIN_VRAM / _GTT / _GDS / _OA.
                                         * radv_amdgpu_bo.c:566-592. GDS and OA are a gap on
                                         * WDDM - see README-winsys.md. */
    __u32 reserved0;
    __u64 gem_flags;                    /* AMDGPU_GEM_CREATE_*. radv_amdgpu_bo.c:595-630 sets
                                         * CPU_ACCESS_REQUIRED, NO_CPU_ACCESS, CPU_GTT_USWC,
                                         * EXPLICIT_SYNC, VM_ALWAYS_VALID, VRAM_CLEARED,
                                         * DISCARDABLE, ENCRYPTED. Not all have a WDDM meaning;
                                         * the ones that do not are recorded and ignored, never
                                         * silently dropped. */

    /* --- the VA request, which amdgpu carries in a separate VA_OP ioctl -----------------------
     * On Linux radv_amdgpu_bo.c:470 calls ac_drm_va_range_alloc(), which is a USER-SPACE
     * allocator in libdrm - no ioctl, no kernel involvement - and then maps at that address.
     * The address is therefore chosen before the kernel hears about it. That is the model this
     * contract has to preserve; BC250_UMD_A_EXACT_VA is how. */
    __u64 requested_va;                 /* the address the winsys already committed to. 0 with
                                         * BC250_UMD_A_EXACT_VA clear means "any". */
    __u64 va_size;                      /* usually alloc_size; differs for sparse */
    __u64 va_flags;                     /* AMDGPU_VM_PAGE_READABLE / _WRITEABLE / _EXECUTABLE /
                                         * _PRT. radv_amdgpu_bo.c:41-47, 173.
                                         * NOT the carrier for the map path. WDDM already has a
                                         * better one: D3DDDI_MAPGPUVIRTUALADDRESS.DriverProtection
                                         * is a 64-bit opaque value passed straight to
                                         * DxgkDdiUpdatePageTable for exactly that range's PTEs
                                         * (m7-full-wddm-miniport.md section 3.2). These flags
                                         * vary per mapping, not per allocation -
                                         * radv_amdgpu_bo.c:213 passes RADEON_FLAG_GL2_BYPASS per
                                         * BO into a VA_OP_REPLACE - so a per-allocation field
                                         * could not express them anyway. This field records the
                                         * creating winsys's intent; the mapping uses
                                         * DriverProtection. See README-winsys.md. */
    __u64 va_offset;                    /* offset into the BO, for partial binds */

    /* --- metadata, ac_drm_bo_set_metadata / bo_query_info (radv_amdgpu_bo.c:1155, :1166) ------
     * On Linux this is an opaque tiling/format blob the kernel stores per BO and hands back to
     * whoever imports it. WDDM has its own per-allocation private data, so this exists to keep
     * an imported BO's description intact across the boundary. */
    __u32 metadata_size;                /* bytes of metadata[] that are meaningful */
    __u32 metadata[16];

    /* --- appended in version 3, inside version 1's reserved tail, so the wire size never changes --
     * The scanned-out surface this allocation holds, for BC250_UMD_A_SCANOUT only. The kernel
     * driver has no other way to learn it: a BC2A allocation carries bytes and a heap, while the
     * display pipeline needs a geometry, a pitch and a pixel format. The format is a D3DDDIFORMAT,
     * as LB7A's is, and must be a SCANOUT_PRIMARY row of driver/contract/amdgpu_wddm_surface_format.h.
     * Zero in all four with the flag clear; a version 2 producer leaves them zero by construction. */
    __u32 scanout_width;                /* pixels; must equal the POST mode's width */
    __u32 scanout_height;               /* pixels; must equal the POST mode's height */
    __u32 scanout_pitch;                /* bytes per row, as the image was laid out */
    __u32 scanout_format;               /* D3DDDIFORMAT, a SCANOUT_PRIMARY row */

    __u32 reserved[7];
};

/* ---------------------------------------------------------------------------------------------
 * 2. Context.  D3DKMTCreateContextVirtual -> DxgkDdiCreateContext
 *
 * amdgpu: ac_drm_cs_ctx_create2(priority) at radv_amdgpu_cs.c:1528. A context there is a
 * scheduling entity with a priority; the IP type is chosen per submission, not per context.
 * WDDM binds a context to a node at creation, so ip_type/ip_instance move up into this blob and
 * the submission blob repeats them for the parity check.
 * ------------------------------------------------------------------------------------------- */

/* bc250_umd_context_private::flags */
#define BC250_UMD_C_STABLE_PSTATE   0x00000001u  /* stable_pstate below is meaningful.
                                                  * ac_drm_cs_ctx_stable_pstate,
                                                  * radv_amdgpu_cs.c:1641. */
#define BC250_UMD_C_RESERVE_VMID    0x00000002u  /* ac_drm_vm_reserve_vmid,
                                                  * radv_amdgpu_winsys.c:211. OPEN QUESTION:
                                                  * VidMm owns VMIDs on Windows. */

struct bc250_umd_context_private {
    __u32 magic;                        /* BC250_UMD_CONTEXT_MAGIC */
    __u32 version;                      /* BC250_UMD_CONTEXT_VERSION */
    __u32 size;
    __u32 flags;                        /* BC250_UMD_C_* */

    __u32 ip_type;                      /* AMDGPU_HW_IP_GFX. On GFX1013 this is the only choice:
                                         * ac_gpu_info.c:501-504 drops AMD_IP_COMPUTE for this
                                         * chip, so a compute workload runs on the gfx ring
                                         * (fact M50). AMDGPU_HW_IP_COMPUTE here is a bug, not a
                                         * preference, and the KMD should refuse it. */
    __u32 ip_instance;                  /* 0; HW_IP_COUNT measured 1 for GFX */
    __u32 ring;                         /* 0; available_rings measured 0x1 for GFX */
    __u32 priority;                     /* AMDGPU_CTX_PRIORITY_*. radv_amdgpu_winsys.h:92-98
                                         * maps VK_EXT_global_priority onto these. WDDM has its
                                         * own scheduling priority; this is the requested value,
                                         * and the KMD decides how to express it. */
    __u32 stable_pstate;                /* AMDGPU_CTX_STABLE_PSTATE_*, radv_amdgpu_cs.c:1609-1617.
                                         * Used by RGP capture. Only if BC250_UMD_C_STABLE_PSTATE. */
    __u32 reserved[7];

    /* --- appended in version 2, strictly after version 1's last byte -------------------------
     * D3DKMTCreateContextVirtual already takes the WDDM NodeOrdinal as its own argument (m7
     * section 3.5), not through this blob - a context's node is not something the private data
     * chooses. node_ordinal exists purely as a cross-check: the KMD compares it against the
     * DDI's own NodeOrdinal and refuses DxgkDdiCreateContext (one of the DDIs allowed to fail;
     * ADR 0008 point 5's never-fail list does not include it) on a mismatch, catching a winsys
     * bug where ip_type (AMDGPU_HW_IP_GFX/_DMA, chosen from RADV's own IP type) and the node the
     * caller actually opened have drifted apart - the two must name the same queue family. See
     * docs/design/umd-contract-stage-d.md. */
    __u32 node_ordinal;                 /* BC250_WDDM_NODE_3D or BC250_WDDM_NODE_COPY, matching
                                         * the value the UMD is about to pass, or already passed,
                                         * to D3DKMTCreateContextVirtual's own NodeOrdinal. */
    __u32 reserved_v2[3];
};

/* ---------------------------------------------------------------------------------------------
 * 3. Submission.  D3DKMTSubmitCommand -> DxgkDdiSubmitCommandVirtual
 *
 * This is the blob with the real semantic gap behind it, and it is worth stating plainly.
 *
 * amdgpu has ONE ioctl: ac_drm_cs_submit_raw2() at radv_amdgpu_cs.c:1849 takes an array of
 * chunks and performs wait, submit and signal atomically. RADV builds exactly five chunk kinds
 * (radv_amdgpu_cs.c:1755-1835):
 *
 *   AMDGPU_CHUNK_ID_IB                     one per IB, :1755
 *   AMDGPU_CHUNK_ID_FENCE                  the completion fence, :1776
 *   AMDGPU_CHUNK_ID_SYNCOBJ_IN / _OUT      binary semaphores, :1799 / :1817
 *   AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT / _SIGNAL  timeline semaphores, :1796 / :1814
 *   AMDGPU_CHUNK_ID_BO_HANDLES             the residency list, :1832
 *
 * WDDM splits that into three calls - SubmitWaitForSyncObjectsToHwQueue, SubmitCommand,
 * SubmitSignalSyncObjectsToHwQueue - and moves residency out of the submission path entirely.
 * So this blob carries only the IB array; the rest is expressed in WDDM's own terms by the
 * winsys. README-winsys.md "Open questions" has what that costs.
 *
 * The IB array itself is the easy part: it is drm_amdgpu_cs_chunk_ib relocated into private
 * data, which is also what Collabora's draft did (m7-full-wddm-miniport.md section 4.3).
 * ------------------------------------------------------------------------------------------- */

/* bc250_umd_submit_private::flags */
#define BC250_UMD_S_PREAMBLE_FIRST  0x00000001u  /* ib[0] is a preamble IB (state setup) and may
                                                  * be skipped when the previous submission on
                                                  * this context left the same state. */
#define BC250_UMD_S_NEED_CTX_SWITCH 0x00000002u  /* the address space may have changed since the
                                                  * last submission on this engine. Named because
                                                  * DxgkDdiSubmitCommandVirtual's documentation
                                                  * puts the burden on the driver: "The GPU might
                                                  * have previously worked with a different
                                                  * address space ... the driver is responsible
                                                  * for making sure the correct address space is
                                                  * restored" (m7 section 3.5). */

/* One entry of the IB array. drm_amdgpu_cs_chunk_ib, field for field. */
struct bc250_umd_ib {
    __u64 va_start;                     /* GPU VA of the IB. Must already be mapped. */
    __u32 ib_bytes;                     /* size in bytes, not dwords */
    __u32 ip_type;                      /* AMDGPU_HW_IP_GFX */
    __u32 ip_instance;
    __u32 ring;
    __u32 ib_flags;                     /* AMDGPU_IB_FLAG_PREEMPT / AMDGPU_IB_FLAGS_SECURE,
                                         * radv_amdgpu_cs.c:1112, :1115. Neither applies on this
                                         * part: preemption is off (measured ids_flags 0x11 has
                                         * no AMDGPU_IDS_FLAGS_PREEMPTION) and TMZ is disabled. */
    __u32 reserved;
};

/* The measured reference shape. E14's command stream capture (fact M50) shows two IBs per
 * dispatch on the gfx ring. Sized well above that so a chained or multi-dispatch submission does
 * not need a version bump; max_submitted_ibs[AMD_IP_GFX] is 192 on this part (measured, caps
 * blob), which is the real ceiling and is deliberately NOT what this array is sized to - a blob
 * that large has no business crossing the boundary in one piece. */
#define BC250_UMD_SUBMIT_MAX_IBS    16u

struct bc250_umd_submit_private {
    __u32 magic;                        /* BC250_UMD_SUBMIT_MAGIC */
    __u32 version;                      /* BC250_UMD_SUBMIT_VERSION */
    __u32 size;                         /* the WHOLE blob as submitted, including only the IB
                                         * entries actually used. The KMD validates
                                         * size == offsetof(ib) + num_ibs * sizeof(ib[0]) before
                                         * reading anything else: DxgkDdiSubmitCommandVirtual is
                                         * the only place a WDDM driver is ALLOWED to reject
                                         * malformed private data (m7 section 3.5), and with no
                                         * UMD of our own it is the only validation hook we get. */
    __u32 flags;                        /* BC250_UMD_S_* */

    __u32 ip_type;                      /* must equal the context's; carried so the KMD can check */
    __u32 num_ibs;                      /* 1..BC250_UMD_SUBMIT_MAX_IBS */

    /* The completion fence, amdgpu's AMDGPU_CHUNK_ID_FENCE (radv_amdgpu_cs.c:1776). On WDDM this
     * is a monitored fence whose value the command stream writes itself, so what the KMD needs is
     * the address and the value, not a handle.
     *
     * This blob is UNIDIRECTIONAL. Learn is explicit that a KMD cannot return anything to user
     * mode through D3DKMTSubmitCommand's private data (m7 section 3.5). ac_drm_cs_submit_raw2()
     * does return a sequence number (radv_amdgpu_cs.c:1849), so there is no field here for it and
     * there must not be one: the winsys mints it from fence_value, which it chose itself. Add an
     * escape if something ever genuinely needs a value back - escape private data IS in/out. */
    __u64 fence_va;                     /* GPU VA the IB writes the completion value to */
    __u64 fence_value;                  /* also the winsys's submit sequence number; see above */

    struct bc250_umd_ib ib[BC250_UMD_SUBMIT_MAX_IBS];

    __u32 reserved[6];
};

/* ---------------------------------------------------------------------------------------------
 * Compile-time checks. Same C89 negative-array-size form as the caps blob, so these hold under
 * /kernel too, where <assert.h> does not exist.
 * ------------------------------------------------------------------------------------------- */
#define BC250_SUBMIT_CTASSERT3(cond, line) \
    typedef char bc250_submit_ctassert_##line[(cond) ? 1 : -1]
#define BC250_SUBMIT_CTASSERT2(cond, line) BC250_SUBMIT_CTASSERT3(cond, line)
#define BC250_SUBMIT_CTASSERT(cond)        BC250_SUBMIT_CTASSERT2(cond, __LINE__)

#ifndef offsetof
#define offsetof(t, m) ((unsigned long)(unsigned long long)&(((t *)0)->m))
#endif

/* The envelope: the first three words of each blob, in the same place in all three, because the
 * KMD reads magic/version/size before it knows which DDI it is in. */
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_alloc_private,   magic)   == 0);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_alloc_private,   version) == 4);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_alloc_private,   size)    == 8);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_context_private, magic)   == 0);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_context_private, version) == 4);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_context_private, size)    == 8);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_submit_private,  magic)   == 0);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_submit_private,  version) == 4);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_submit_private,  size)    == 8);

/* The three magics must differ, or the cross-DDI check above is decorative. */
BC250_SUBMIT_CTASSERT(BC250_UMD_ALLOC_MAGIC != BC250_UMD_CONTEXT_MAGIC);
BC250_SUBMIT_CTASSERT(BC250_UMD_ALLOC_MAGIC != BC250_UMD_SUBMIT_MAGIC);
BC250_SUBMIT_CTASSERT(BC250_UMD_CONTEXT_MAGIC != BC250_UMD_SUBMIT_MAGIC);

/* 8-byte members must land on 8-byte offsets under every packing mode the KMD builds with. */
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_alloc_private, alloc_size) % 8 == 0);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_alloc_private, requested_va) % 8 == 0);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_submit_private, fence_va) % 8 == 0);
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_ib, va_start) % 8 == 0);
BC250_SUBMIT_CTASSERT(sizeof(struct bc250_umd_ib) == 32);

/* Sizes, so an edit that changes the wire format has to say so here too. */
#define BC250_UMD_ALLOC_SIZE_V1     192
#define BC250_UMD_CONTEXT_SIZE_V1   64
#define BC250_UMD_CONTEXT_SIZE_V2   80
#define BC250_UMD_SUBMIT_SIZE_V1    576
BC250_SUBMIT_CTASSERT(sizeof(struct bc250_umd_alloc_private)   == BC250_UMD_ALLOC_SIZE_V1);
BC250_SUBMIT_CTASSERT(sizeof(struct bc250_umd_context_private) == BC250_UMD_CONTEXT_SIZE_V2);
BC250_SUBMIT_CTASSERT(sizeof(struct bc250_umd_submit_private)  == BC250_UMD_SUBMIT_SIZE_V1);
/* Version 2 grew only at the end: node_ordinal starts exactly where version 1's reserved tail
 * stopped, so a version-1 reader (there are none yet, but the rule is the same as the caps blob's)
 * still finds every field it knows where it expects it. */
BC250_SUBMIT_CTASSERT(offsetof(struct bc250_umd_context_private, node_ordinal) == BC250_UMD_CONTEXT_SIZE_V1);

/* A submission blob is copied per submit, on the hot path. Keep it small enough that the copy is
 * never the reason a submission is slow.
 *
 * 1024 is our own ceiling, not the real one. The binding limit is whatever the KMD asks for in
 * DXGK_CONTEXTINFO at context creation: D3DKMTSubmitCommand fails outright if the private data
 * exceeds it (m7-full-wddm-miniport.md section 3.5). When the KMD side exists, this assert should
 * be restated against that constant so the two cannot drift apart. */
BC250_SUBMIT_CTASSERT(BC250_UMD_SUBMIT_SIZE_V1 <= 1024);

#if defined(__cplusplus)
}
#endif

#endif /* BC250_UMD_SUBMIT_H */
