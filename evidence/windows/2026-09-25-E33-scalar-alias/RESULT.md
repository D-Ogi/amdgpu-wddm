# M485: canonical scalar aliases and release sparse-buffer CTS

Unit A, 2026-09-25. KMD149 remains SYS93ECB1BE. Mesa05e6c962 candidate
17ADD01FDEB9B7E4530F19C8E5282406D66F607D71C1B8D6D5B34A24D623E481.
Sparse remains disabled by default; this run explicitly uses RADV_EXPERIMENTAL=sparse.
This is development correctness evidence, not formal conformance or M12 acceptance.

## Results
- 21/21 release CTS cases pass, zero NotSupported, vulkan-cts-1.4.6.2
  at f6a29701220f34dd1407513bfe80d74ca7b392ce.
- Corpus: two bound SSBO alias sizes (64KiB/16MiB), sixteen null-address
  read/write variants (uniform/divergent indexing, BDA/descriptors, initial
  holes/unmap-after-binding), two sparse SSBO residency sizes and strict read/write.
- Native actual-source helpers pass20 exact comparisons across both views:
  bound A, rebound B, initial/unmapped zeros and high-hole write discard.
  Application wait is delayed; both views update before the final queue fence.
- Host model covers delayed/already-signaled waits, ordinary/paired views,
  packet/HW queue APIs,257-operation batches and cancellation. Reservation controls
  cover64KiB, interior512KiB alignment,5GiB and exact replay.
- Intermediate C07962AB candidate passes ordinary enumeration,8 reference compute
  hashes and600cube frames. Final17ADD01F adds the missing packet-queue paging
  fence; its21CTS passes are separate evidence, not a rerun of that ordinary suite.
- System ICD9C40083C restored after each temporary elevated CTS registration.
  Same boot14:50:35Z throughout paired/CTS controls; no new selected fault events.

## Implementation
The compiler's existing NULL-PRT SMEM lowering clears control bit46. Ordinary,
imported and capture/replay allocations stay below that bit; replay starts at32TiB.
Sparse residency buffers own a high PRT view and a low real-zero view. Both
reservations are OS-owned and freed with the resource. An8MiB shared read-only
zero BO is explicitly CPU-initialized and store-barriered; BC2A has no ZERO_VRAM
initialization contract. It lives until winsys destruction.

WDDM requires every operation in one UpdateGpuVirtualAddress call to belong to
one ReserveGpuVirtualAddress reservation. Operations are therefore grouped by
reservation, preserving order within each resource/view, then chained on one
dedicated monitored fence. Rendering waits once for all groups. The packet
queue creates this paging fence separately from its IB-reuse progress fence.

Source references: local d3dukmdt.md D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION,
d3dkmthk.md D3DKMT_UPDATEGPUVIRTUALADDRESS (WDK26100), MS display/tile-resources.md;
Mesa radv_amdgpu_bo.c and ac_nir_fixup_smem_loads_null_prt.c at the pinned commit.
The full port patch is replay-verified against the pinned base (75 extant files).

## Failed controls retained
- Bit47: paired reservations and initial maps succeed as API calls, but access
  to the noncanonical high range triggers TDR0x116 and automatic reboot14:50:35Z.
  Physical low A/B controls pass before that failure. Linux amdgpu_kms.c and
  AMDGPU_GMC_HOLE_START identify the canonical boundary. Bit46 positive controls
  then pass on the unchanged KMD. No power-plug action; full new dump was not copied.
- First paired update incorrectly spans two reservations: STATUS_INVALID_PARAMETER,
  with successful initial mappings and physical controls. The reservation grouping
  fixes that contract violation without a KMD change.
- First Vulkan candidate reaches QueueBindSparse but returns DeviceLost because
  the BC250 packet-queue creation returns before creating vm_fence. The dedicated
  fence fix produces the21 passing results; no TDR or OS restart in either CTS run.

## Limits
Native paired controls are DMA_DATA, not proof of every shader instruction.
CTS now covers scalar/vector buffer behavior; images, complete release must-pass,
same-unit Linux parity and queued privileged PTE relocation remain open.
KMD still snapshots physical PTE inputs during construction; the MS late-binding
contract must be completed before general sparse acceptance. COPY_DATA holes from
M484 remain a separate unresolved path. No full-system certification claim.

Raw stdout/CTS QPA/scripts/source are in controls.zip with hashes in manifest.json.
Adapter LUIDs, device UUIDs and WER report IDs are redacted; raw originals remain
outside the repository. No memory dump bytes are included.
