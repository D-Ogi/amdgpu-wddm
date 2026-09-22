# driver/contract - the winsys contract

What RADV needs from the kernel *after* it knows what the GPU is, and what carries each of those
things on WDDM.

`README.md` and `bc250_umd_private.h` cover the caps blob: one query, answered once at adapter
open. This document covers the rest of the surface - allocation, address space, contexts,
submission, synchronisation - and `bc250_umd_submit.h` is the shape it produces.

**Status: the kernel reads the three blobs** (`driver/kmd/umd_blob.c`, node 0 only, host-tested).
The winsys that fills them is not written yet.

## Scope: what a compute-only workload on GFX1013 actually touches

Narrowing this hard is what makes the surface tractable:

- **The gfx ring, and only the gfx ring.** `ac_gpu_info.c:501-504` drops `AMD_IP_COMPUTE` outright
  for `FAMILY_NV` in the GFX1013 range, so RADV sees zero compute queues no matter what the KMD
  advertises (measured: `radv-info.txt` has an `IP GFX` line and an `IP SDMA` line, and no
  `IP COMPUTE` line at all). A compute dispatch runs on the gfx ring. Fact M50.
- **Two IBs per dispatch**, from E14's command stream capture
  (`evidence/linux/2026-09-21-E14-vulkan-compute-reference/cs/`). `bc250_umd_submit.h` sizes its
  IB array at 16 for headroom, not at the measured `max_submitted_ibs[GFX]` of 192.
- **No video, no display, no sparse-heavy paths.** Every video IP answered `HW_IP_INFO` with a
  zero word; there is no KMS to integrate with.
- Preemption and TMZ are both off (measured `ids_flags` = `0x11`, neither bit set), so
  `AMDGPU_IB_FLAG_PREEMPT` and `AMDGPU_IB_FLAGS_SECURE` never appear.
- **Node 1 (SDMA0, the ADR 0013 copy/paging node) is not exposed as a queue family in M8.** ADR
  0013 point 3 wrote "the ICD exposes node 1 as its transfer queue family" before
  `docs/design/paging-node.md` decided that node 1's hardware path carries only physical MC
  addresses and never reprograms a VMID - which means it cannot run a UMD-submitted, GPU-VA-
  addressed buffer at all today (`Bc250WddmSubmitCommandVirtual` explicitly excludes node 1 from
  its hardware path; a context opened there would have every fence completed in software, silently,
  without reaching the GPU). `docs/design/umd-contract-stage-d.md` section 1a has the full
  reasoning and the recommendation: RADV uses the gfx ring only, which costs nothing RADV would
  notice, since fact M50 already has it dropping compute queues and running everything on the gfx
  ring on this chip. The caps blob's `submittable_node_mask` (version 3) says so in a form a winsys
  can check rather than assume: bit 1 (node 1) is clear until a VMID-addressed submit path for it
  exists.

## The call set, from RADV's own source

`docs/research/m7-full-wddm-miniport.md` section 3.5 maps D3DKMT calls to DDIs. This table starts
one layer higher - at the call sites in `P:\BC-250\ref\mesa\src\amd\vulkan\winsys\amdgpu` - so
that a port has a checklist of things RADV actually invokes rather than a list of things WDDM
offers. The DDI column defers to section 3.5 rather than repeating it.

### Device and adapter

| RADV call | amdgpu ioctl | WDDM carrier | Notes |
|---|---|---|---|
| `ac_drm_device_initialize` (`winsys.c:231`) | open `/dev/dri/renderD128` | `OpenAdapterFromLuid` + `CreateDevice` | |
| `ac_query_gpu_info` (`winsys.c:341-342`) | `INFO_DEV_INFO`, `INFO_HW_IP_INFO`, `INFO_FW_VERSION`, `INFO_MEMORY` | caps blob | **done**, see `README.md` |
| `ac_drm_read_mm_registers` (`winsys.c:81`) | `INFO_READ_MMR_REG` | caps blob `tiling.gb_addr_config` | **done** - and see `README.md` point 3, this one is not a register read |
| `ac_drm_query_heap_info` (`winsys.c:52,397`) | `INFO_MEMORY` | `QueryVideoMemoryInfo` | **not** the blob: RADV re-queries this live for `VK_EXT_memory_budget`. The blob's totals are fixed; the `heap_usage` / `max_allocation` fields move, and VidMm owns them on Windows |
| `ac_drm_query_info(TIMESTAMP)` (`winsys.c:40`) | `INFO_TIMESTAMP` | `DxgkDdiCalibrateGpuClock` | live GPU clock for `VK_EXT_calibrated_timestamps`, not a caps value |
| `ac_drm_query_info` counters (`winsys.c:43,46,49`) | `INFO_NUM_BYTES_MOVED`, `_NUM_EVICTIONS`, `_NUM_VRAM_CPU_PAGE_FAULTS` | `QueryStatistics`, or an escape | live residency counters, reported through `VK_EXT_memory_budget` and RGP |
| `ac_drm_query_info(GPUVM_FAULT)` (`winsys.c:91`) | `INFO_GPUVM_FAULT` | escape, after a TDR | returns addr/status/vmhub of the last VM fault for device-lost reporting. Needs the KMD to latch the fault, since dxgkrnl resets the engine before the UMD asks |
| `ac_drm_query_sensor_info` (`winsys.c:61-67`) | `INFO_SENSOR` | escape | telemetry only, not on the compute path |
| `ac_drm_vm_reserve_vmid` / `unreserve` (`winsys.c:211,218`) | `AMDGPU_VM` | **nothing** | open question 5 |

### Buffer objects

| RADV call | amdgpu ioctl | WDDM carrier | Notes |
|---|---|---|---|
| `ac_drm_bo_alloc` (`bo.c:633`) | `GEM_CREATE` | `CreateAllocation2` + `bc250_umd_alloc_private` | heap from `bo.c:566-592`, flags from `:595-630` |
| `ac_drm_bo_free` (`bo.c:388,698`) | `GEM_CLOSE` | `DestroyAllocation2` | |
| `ac_drm_bo_cpu_map` / `unmap` (`bo.c:730,769`) | `GEM_MMAP` + `mmap` | `Lock2` / `Unlock2` | on a GpuMmu device these need no DDI (section 3.5) |
| `ac_drm_create_bo_from_user_mem` (`bo.c:819`) | `GEM_USERPTR` | ? | open question 4 |
| `ac_drm_bo_export` / `import` (`bo.c:644,900`) | PRIME dma-buf | `ShareObjects` / `OpenResource` | not needed for a single-process compute job |
| `ac_drm_bo_query_info` (`bo.c:910,1166`) | `GEM_OP` | `DescribeAllocation` + our blob | |
| `ac_drm_bo_set_metadata` (`bo.c:1155`) | `GEM_METADATA` | `bc250_umd_alloc_private.metadata` | |
| `ac_drm_bo_wait_for_idle` (`bo.c:1304`) | `GEM_WAIT_IDLE` | monitored fence wait | |

### Address space - the part that decides whether the port is honest

| RADV call | amdgpu ioctl | WDDM carrier | Notes |
|---|---|---|---|
| `ac_drm_va_range_alloc` (`bo.c:470,551,830,920`) | **none** | none | see below |
| `ac_drm_bo_va_op_raw` (`bo.c:77`) | `GEM_VA` | `MapGpuVirtualAddress` / `Update...` / `Free...` | ops at `bo.c:59,181,192,213,387`: `MAP`, `UNMAP`, `REPLACE`, `CLEAR` |
| `ac_drm_bo_va_op_raw2` (`bo.c:65`) | `GEM_VA` with an out-fence | the paging queue's monitored fence | `raw2` takes a syncobj + timeline point and `bo.c:73` then waits on it - structurally the same as waiting on a WDDM paging fence |
| PRT / sparse (`bo.c:173,237`) | `VA_OP_MAP` with `AMDGPU_VM_PAGE_PRT` | `ReserveGpuVirtualAddress` then `MapGpuVirtualAddress` with `hAllocation == 0`, `Protection.Zero = 1` | confirms m7 section 4.3's reading |

**`ac_drm_va_range_alloc` issues no ioctl.** It is a user-space allocator inside libdrm/`ac_drm`;
RADV chooses every GPU virtual address itself and only then tells the kernel to map there. The
address is baked into descriptors before the kernel has heard of it, so "VidMm picked a different
address" is not a recoverable outcome - it is a corrupt descriptor set.

This confirms, from the RADV side, what m7 section 4.3 found on the Collabora side: their draft
commented out `radv_wddm2_bo_va_alloc()` (`m7:1027-1031`, zero live callers) and let VidMm choose.
Section 3.2's verdict - RADV can keep its own allocator, by reserving 64 KB-aligned windows and
placing allocations inside them - is the right one, and `BC250_UMD_A_EXACT_VA` is how the blob
demands it. **This is the single design decision that keeps RADV's memory model intact**, and it
is the reason the allocation blob carries `requested_va` at all.

Two consequences of section 3.2 that the winsys has to absorb, both of them ours to handle
because RADV will not:

- **Reserve wants 64 KB alignment and a 64 KB size multiple; Map wants only 4 KB** (`m7:734-735`).
  RADV's allocator aligns to `vm_alignment` / `virt_alignment`, which is page-sized in the general
  case. So the windows we Reserve must be rounded out to 64 KB even when RADV's own range is not,
  and the winsys - not the KMD - has to own that rounding.
- **`VirtualAddress` must be read back.** Learn promises only that the call fails when the address
  cannot be honoured, never that `VirtualAddress == BaseAddress` (`m7:749-751`). A winsys that
  assumes the identity silently corrupts descriptors on the day the assumption breaks; one that
  compares and hard-fails turns it into a clean error. `BC250_UMD_A_EXACT_VA` should be read as
  "fail the call rather than relocate", not as "trust me".

**`DriverProtection` is the carrier for AMD's page flags, and it is better than our blob field.**
Section 3.2 establishes (`m7:722-728`) that `D3DDDI_MAPGPUVIRTUALADDRESS.DriverProtection` is a
64-bit opaque value handed straight to `DxgkDdiUpdatePageTable` for exactly the PTEs of that
range. That is a per-PTE hook from the ICD to our page-table code, which is precisely what
`AMDGPU_VM_PAGE_READABLE` / `WRITEABLE` / `EXECUTABLE` / `PRT` / `MTYPE_*` need and what the
standard `D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE` enum cannot express. `bc250_umd_alloc_private`
carries `va_flags` as well, which is now redundant for the map path; it is kept for the
create-time record of intent, but **the map path should use `DriverProtection`**, since it is
per-range rather than per-allocation and survives `UpdateGpuVirtualAddress`.

**The VA space is per process, not per device** (`m7:738-741`). amdgpu's is per `drm_file` / VM,
so a process that opened two devices would get two independent VA spaces on Linux and one shared
space on Windows. Irrelevant for a single-device compute job; it would matter for multi-GPU, and
RADV's fixed heap layout is what would collide.

### Contexts

| RADV call | amdgpu ioctl | WDDM carrier | Notes |
|---|---|---|---|
| `ac_drm_cs_ctx_create2` (`cs.c:1528`, `winsys.c:311`) | `CTX` op `ALLOC` | `CreateContextVirtual` + `bc250_umd_context_private` | priority from `winsys.h:92-98` |
| `ac_drm_cs_ctx_free` (`cs.c:1551`) | `CTX` op `FREE` | `DestroyContext` | |
| `ac_drm_cs_ctx_stable_pstate` (`cs.c:1641`) | `CTX` op `SET_STABLE_PSTATE` | escape | RGP capture only |
| `ac_drm_cs_query_fence_status` (`cs.c:1591`) | `WAIT_CS` / `FENCE_TO_HANDLE` | monitored fence | |

### Synchronisation

Every one of these maps onto a monitored fence, and - per `m7:910` - none of them reaches a DDI
at all: dxgkrnl's scheduler owns them. That makes this the cheapest section of the port, and the
reason is `has_timeline_syncobj`, which the caps comparison flagged as differing on the device.

**A D3DKMT monitored fence and one of the KMD's own internal fence slots are not the same thing,
and the contract only ever exposes the former.** `bc250_umd_submit_private.fence_va`/`fence_value`
name a monitored fence - the GPU VA the IB itself writes and the value it writes there
(`D3DDDI_MONITORED_FENCE`, created through `CreateSynchronizationObject2`) - and the winsys reads
it back with zero syscalls, as the Submission section below describes. The KMD separately keeps a
small, fixed number of hardware fence-page slots per ring for its own bookkeeping
(`BC250_PAGING_FENCE_SLOT`, `BC250_SUBMIT_FENCE_SLOT`, `docs/design/paging-node.md` section 5) -
these are never named in any contract blob and a UMD has no business knowing they exist; they are
how the KMD polls hardware completion, not how a UMD waits for one.

| RADV call | amdgpu ioctl | WDDM carrier | Notes |
|---|---|---|---|
| `ac_drm_cs_create_syncobj2` (`cs.c:1579`) | `SYNCOBJ_CREATE` | `CreateSynchronizationObject2`, type `D3DDDI_MONITORED_FENCE` | RADV passes `DRM_SYNCOBJ_CREATE_SIGNALED` for the per-ring queue syncobj; the WDDM equivalent is an initial fence value, not a flag |
| `ac_drm_cs_destroy_syncobj` (`cs.c:1565`) | `SYNCOBJ_DESTROY` | `DestroySynchronizationObject` | |
| `ac_drm_cs_syncobj_timeline_wait` (`bo.c:73`) | `SYNCOBJ_TIMELINE_WAIT` | `WaitForSynchronizationObjectFromCpu` | comparison is `>=`, and `hAsyncEvent` is the only way to get a timeout (`m7:792-793`) |
| `ac_drm_cs_syncobj_query2` (`cs.c:1198`) | `SYNCOBJ_QUERY` | read `FenceValueCPUVirtualAddress` | no syscall; the mapping is read-only by contract (`m7:774-775`) |
| `ac_drm_cs_syncobj_transfer` (`cs.c:1223,1239`) | `SYNCOBJ_TRANSFER` | no equivalent | RADV uses it to move a point between timelines; on WDDM this has to become a CPU-side signal |
| `ac_drm_cs_syncobj_export_sync_file` (`cs.c:1173,1179,1190`) | `SYNCOBJ_EXPORT_SYNC_FILE` | `ShareObjects` | only for `VK_KHR_external_semaphore_win32`, not for compute |
| `ac_drm_cs_syncobj_import_sync_file` (`cs.c:1210,1232`) | `SYNCOBJ_IMPORT_SYNC_FILE` | `OpenSyncObjectFromNtHandle2` | as above |

`syncobj_transfer` is the only genuine gap. It has no WDDM primitive, and RADV uses it in
`radv_amdgpu_cs_submit`'s semaphore plumbing rather than in an extension path, so it cannot simply
be declared out of scope the way the sync-file pair can. The likely shape is a CPU wait on the
source point followed by a `SignalSynchronizationObjectFromCpu` on the destination, which is
correct but synchronous - worth measuring before assuming it is acceptable.

### Submission

| RADV call | amdgpu ioctl | WDDM carrier |
|---|---|---|
| `ac_drm_cs_submit_raw2` (`cs.c:1849`) | `CS`, one ioctl | `WaitForSynchronizationObjectFromGpu`, then `SubmitCommand` |

Note this is **not** `SubmitCommandToHwQueue` and not the `Submit*SyncObjectsToHwQueue` pair:
those are the hardware-scheduling path, which m7 section 3.5 (`m7:768`) rules out of scope. On the
packet-scheduling path the queue-side signal is not a syscall at all (see below), so the atomic
Linux ioctl decomposes into **two** calls, not three.

The chunks RADV builds, and where each one goes:

| Chunk | Built at | WDDM carrier |
|---|---|---|
| `AMDGPU_CHUNK_ID_IB` | `cs.c:1755` | `bc250_umd_submit_private.ib[]` |
| `AMDGPU_CHUNK_ID_FENCE` | `cs.c:1776` | `fence_va` / `fence_value` - the IB writes it itself |
| `SYNCOBJ_IN` / `SYNCOBJ_TIMELINE_WAIT` | `cs.c:1799` / `:1796` | `WaitForSynchronizationObjectFromGpu` before the submit |
| `SYNCOBJ_OUT` / `SYNCOBJ_TIMELINE_SIGNAL` | `cs.c:1817` / `:1814` | **no syscall** - a `RELEASE_MEM` in the IB writes `FenceValueGPUVirtualAddress` |
| `AMDGPU_CHUNK_ID_BO_HANDLES` | `cs.c:1832` | **no carrier** - open question 2 |

**The signal side is free, and that is the nicest result in this document.** `m7:776-780`: the
GPU has a read/write mapping of the monitored fence and "the UMD inserts a fence write command in
a context command stream directly without going through kernel mode". RADV already emits exactly
that packet for `AMDGPU_CHUNK_ID_FENCE`, so `SYNCOBJ_OUT`, `SYNCOBJ_TIMELINE_SIGNAL` and the
fence chunk all collapse into one `RELEASE_MEM` the IB was going to carry anyway.
`vkGetSemaphoreCounterValue` then reads `FenceValueCPUVirtualAddress` with zero syscalls
(`m7:774-775`, `m7:785`).

Three constraints on `bc250_umd_submit_private` that come from the WDDM side, not from RADV:

- **The private data is unidirectional** (`m7:759-761`): "the kernel mode driver can't return
  information to the user mode driver through this buffer". But `ac_drm_cs_submit_raw2` returns
  `&request->seq_no` (`cs.c:1849`). So the sequence number **cannot** come back from the KMD - the
  winsys has to mint it user-side from the monitored fence value it is about to signal. Since the
  UMD picks `fence_value` anyway, this works, but it has to be designed in rather than discovered.
  Anything genuinely bidirectional needs an escape, whose private data *is* in/out (`m7:797-798`).
- **The size is capped by `DXGK_CONTEXTINFO`**, not by us: the call fails if the private data
  exceeds what the KMD requested at context creation (`m7:761`). `BC250_UMD_SUBMIT_SIZE_V1` is
  576 bytes and the header asserts `<= 1024`; that ceiling is arbitrary today and must be
  reconciled with whatever the KMD advertises.
- **There is no `hContext` member** (`m7:756-757`). The target context is `BroadcastContext[0]`
  with `BroadcastContextCount = 1`, and `NumPrimaries = 0` for compute. The blob does not need a
  context field, and should not grow one.

## Open questions

1. **Submission is not atomic on WDDM, though less badly than it first looks.** One `CS` ioctl
   becomes `WaitForSynchronizationObjectFromGpu` followed by `SubmitCommand`. The signal side
   costs nothing (it rides in the IB), so the window is between the wait and the submit, not
   across three calls. Two things are still unanswered: whether dxgkrnl guarantees the queued
   wait stays ordered ahead of the submit on the same context - it should, both being packets on
   one context's stream, but "should" is why this is listed - and what happens if `SubmitCommand`
   fails *after* the wait is queued, leaving a wait with no signal behind it. The second is the
   real hazard: it is a hang, not an error return.
2. **`BO_HANDLES` has no WDDM carrier, and that is by design.** On Linux the per-submit BO list
   tells the scheduler what must be resident. On WDDM residency is a separate lifecycle -
   `MakeResident` / `Evict` against a paging queue - so the list has nowhere to go in the
   submission blob. The question is who drives it. Collabora dodged it entirely
   (`all_resident = true`, m7 section 4.3), which fails hard under memory pressure. Our options
   are to mirror `cs_add_buffer` into `MakeResident` calls, or to make every allocation resident
   at creation and accept the same ceiling.
3. **Will VidMm's own reservations collide with RADV's fixed heaps?** This is m7's open question
   O4 (`m7:1311`) and this document does not close it, but it can sharpen it. The semantics are
   not in doubt - `BaseAddress` is honoured or the call fails (`m7:716-718`), so there is no
   silent-relocation risk to test for. The risk is availability: RADV's heap bases are compiled-in
   constants, and if VidMm has already reserved a range that overlaps one, every later allocation
   in that heap fails and there is no fallback. What is needed is not a hardware test of
   pass-through but a **map of what VidMm reserves in a fresh process before the ICD runs**,
   compared against RADV's heap layout. That is a measurement nobody has taken, and it is cheap:
   reserve nothing, then walk the space.
4. **userptr.** `ac_drm_create_bo_from_user_mem` wraps existing user pages in a BO. RADV uses it
   for `VK_EXT_external_memory_host`. There is no obvious WDDM equivalent for a render device.
   Not needed for a first compute workload; needs an answer before that extension is claimed.
5. **`vm_reserve_vmid` has no equivalent, and probably needs none.** RADV asks for a dedicated
   VMID; on Windows VidMm owns VMID assignment entirely. The likely answer is that the request is
   recorded and ignored, but "likely" is why this is a question.
6. **GDS and OA.** `bo.c:590-592` can allocate from `AMDGPU_GEM_DOMAIN_GDS` and `_OA`. We
   measured a real GDS (64 KiB, `gws_per_compute_partition` 64, `oa_per_compute_partition` 16),
   but WDDM segments do not naturally express them. Probably out of scope for compute; listed so
   it is not discovered late.
7. **Implicit versus explicit sync.** `AMDGPU_GEM_CREATE_EXPLICIT_SYNC` (`bo.c:603`) opts a BO out
   of implicit fence tracking. WDDM has no implicit sync to opt out of, so the flag is
   meaningless - but confirming it is meaningless rather than assuming so is one read of the
   dma-resv path.
8. **Validation.** `DxgkDdiSubmitCommandVirtual` is the only DDI permitted to reject malformed
   private data - Learn contrasts it with `DxgkDdiSubmitCommand`, "where no error is allowed to be
   returned due to the ability to validate the data in a prior `DxgkDdiRender` call" (`m7:915-919`).
   We have no `DxgkDdiRender` and no UMD, so this is the only validation hook that exists.
   `bc250_umd_submit_private.size` is specified to be checked against `num_ibs` there. What else
   must be validated - that every `ib[].va_start` is mapped, that `ip_type` matches the context -
   is undecided.
9. **Address-space restore on every submit, which Linux never asks for.** The same Learn page
   (`m7:920-921`): "The GPU might have previously worked with a different address space ... The
   driver is responsible for making sure the correct address space is restored ahead of submitting
   a particular DMA buffer." On Linux amdgpu's scheduler owns VMID allocation and VM switching, so
   nothing in the RADV winsys has an opinion about it and there is no field to map. On WDDM it is
   our obligation, discharged in `DxgkDdiSubmitCommandVirtual` before the ring write. Whether the
   KMD can infer the address space from the context handle alone, or whether the submit blob has
   to name it, is the open part - and it is the one question here that could still add a field to
   `bc250_umd_submit_private`.

## What was verified against the existing tables

Sections 3.2, 3.5 and 4.3 of `m7-full-wddm-miniport.md` hold up. Nothing in them was contradicted
by the RADV side. Five things to carry back:

- The sparse path in section 4.3 is confirmed from RADV's side: `bo.c:173` and `:237` use
  `AMDGPU_VM_PAGE_PRT` with `VA_OP_MAP` and `VA_OP_REPLACE`, exactly the pairing that document
  predicted maps onto `MapGpuVirtualAddress(hAllocation == 0, Protection.Zero)` and
  `UpdateGpuVirtualAddress`.
- `ac_drm_bo_va_op_raw2`'s out-fence (`bo.c:65-76`) is a closer analogue of the WDDM paging fence
  than section 3.5's row for `MapGpuVirtualAddress` suggests: RADV already submits the VA
  operation and then waits on a timeline point, which is the shape a paging queue produces.
- The row for `ReserveGpuVirtualAddress` is marked ASSUMPTION ("no PTE writes, so no paging
  buffer"). RADV's use of it is sparse-only, so the assumption is only load-bearing for sparse
  resources - which a first compute workload does not use. Worth re-marking as lower risk.
- **`DriverProtection` deserves promoting from a remark to a design element.** Section 3.2 found
  it; nothing downstream uses it yet. It is the only mechanism that gets AMD's VM page flags to
  our page-table code per range, and the RADV side confirms those flags vary per mapping
  (`bo.c:213` passes `RADEON_FLAG_GL2_BYPASS` per BO), so a per-allocation field would not have
  been sufficient anyway.
- **Open question O4 can be narrowed.** Section 3.2 poses it as "will VidMm honour
  `BaseAddress` for every allocation". The RADV side shows the failure mode is not relocation but
  allocation failure at a fixed heap base, which makes it a question about VidMm's own
  reservations rather than about pass-through fidelity, and answerable without any AMD hardware.
