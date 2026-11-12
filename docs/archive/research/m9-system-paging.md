# M9 system-memory paging implementation work

Status2026-09-23: incomplete design and implementation. This note guides the remaining
work; it is not evidence of hardware support or a replacement for M9 acceptance.

## Constraints established from current source

- Paging currently resolves virtual addresses on the CPU, converts local VRAM physical
  addresses to MC and emits COPY/FILL directly to SDMA0 under VMID0.
- System-memory physical addresses are not that MC namespace. bc250_gmc_setup disables
  the AGP aperture and creates a512MiB GART. gpumem.c uses its first64MiB for driver
  allocations. A separate paging window must be explicitly reserved and checked against
  the actual GART range/table capacity before use.
- Allocating mappings while BuildPagingBuffer runs creates ownership problems: multiple
  OS buffers can remain pending, and discarded buffers have no current mapping-release
  callback. A shared mutable window cannot be reused before its previous GPU consumer.
- Submission may run at DISPATCH_LEVEL. Long CPU TLB polling or GartLock acquisition
  is unsuitable there. Ring ordering and GPU register packets need to carry this work.
- Microsoft BuildPagingBuffer contract requires actual commands; silently skipping
  system pages is not successful paging. Returning an arbitrary new failure code is
  not a substitute for implementing the requested transfer.

## Selected direction to implement and validate

Compose each submitted batch so SDMA writes the reserved GART PTEs, invalidates the
appropriate hub TLB, copies/fills through the window, clears borrowed mappings and
invalidates again before the completion fence. This places mapping lifetime in the
same GPU order as its consumer and avoids retaining mappings for unsubmitted OS
buffers. Explicit per-page values support scattered pages. Private records continue
to own command bytes independently for every OS DMA buffer.

This direction still needs an upstream-based invalidation implementation: establish
which hub SDMA uses, select an invalidation engine with explicit ownership distinct
from concurrent CPU/GFX flushes, and preserve upstream semaphore and acknowledgement
ordering. Do not assume arbitrary register writes, an unused engine number, or cache
coherency. Keep all addresses derived from the imported register tables. An SDMA
IB with a dedicated paging VMID remains an alternative if evidence favors it.

## Implementation and validation sequence

1. DONE locally: PTE WRITE_LINEAR constructor from AMD's sdma_v5_0_vm_write_pte,
   accepting explicit page values;64packet checks pass (factsM175). Not runtime-wired.
2. PARTIAL locally (M176): AMD-derived SDMA request/write/read/ACK constructor,
   GFXHUB engine0 reserved for paging, CPU engine17 separate;122packet checks pass.
   Upstream SDMA5 selects GFXHUB, so no MMHUB semaphore is needed for this path.
   Still establish memory-pipeline synchronization from PTE writes before invalidation
   and from data transfer before unmap, then replay the complete sequence. No CPU
   busy-wait fallback at DISPATCH_LEVEL. Engine0 ownership must remain exclusive when
   future rings acquire GPU-side invalidation support.
3. Reserve and bound the paging GART window without overlap with driver allocations.
   Derive page-entry flags from the existing AMD GART contract, including invalidation.
4. Integrate map/copy/unmap with command-budget and multipass accounting; preserve
   independent pending buffers. Cover local/system, system/local and system/system,
   fragmented lists, partial pages, no-room and malformed records on the host.
5. Hardware positive control: small known patterns in both directions, actual hardware
   completion, zero timeout/refusal, mapping cleanup and a second buffer using the same
   window. Then actual eviction/restore under pressure and1GiB acceptance. CPU readback
   alone and emitted-command coverage alone do not prove GPU paging.

Sources: driver/shim/bc250_gmc.c; driver/kmd/gpumem.c; imported AMD
reference/sdma_v5_0.c:sdma_v5_0_vm_write_pte and ring_emit_vm_flush;
local Microsoft nc-d3dkmddi-dxgkddi_buildpagingbuffer.md at revision
7515063cea4c9e98db6a92986c5b4ddb0463fd16.

## Combined constructor (M177, local only)

bc250_sdma_paging_mapped_copy now constructs the whole map/fence-poll/invalidate/
copy/fence-poll/unmap/fence-poll/invalidate sequence. It reserves the complete command
budget before writing output. Two mapped pages require83DWORDs (96aligned), plus
the eventual outer completion fence/padding.335host assertions pass, including
existing packet controls. This is a candidate packet sequence, not hardware proof.

Next integration must reserve a window and a separate scratch marker slot. Marker
values must be fresh relative to prior contents and distinct between phases; a
bounded one-in-flight batch can reset a driver-owned uncached marker slot only AFTER
the preceding actual GPU fence, then use increasing nonzero markers within that
batch. Timeout must prevent reuse. Do not reset marker memory while GPU readers may
still exist. Source/destination/window bounds and PTE flags are caller obligations.
The constructor currently rejects marker wrap and handles copy only; fill and mixed
local/system routing need integration with the page stream and its budgets.

## Local integration0766 (M178)

Virtual paging resolver/emitter now invokes bc250_sdma_paging_mapped_transfer for
system/local, local/system, system/system and system fill. The two-page window is
reserved immediately above the shared64MiB driver GTT limit and bounded against
aperture/table sizes. Scratch fence slot5 is reset only after claiming an idle SDMA
submission; marker ranges derive from command offsets including prior build calls.
The actual page-stream builder retains multipass behavior with the larger transaction
budget. Local/local emits direct packets.364host routing/packet/stream assertions pass,
plus bounds and builder-lifetime tests; the signed0766candidate is not deployed.

Remaining: OS page-lifetime and legacy physical ADL review, error/refusal policy,
hardware cache/TLB and scratch-coherency controls, real paging/eviction and1GiB.
Earlier statements about unconnected helpers describe prior revisions only.
