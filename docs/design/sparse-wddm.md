# M12.1 - Sparse resource support on WDDM

Status: implementation in progress,2026-09-25. Part of M12 Vulkan capabilities
and conformance, not a limitation imposed by the WDDM2 standard. See M481/M482
in docs/facts.md for current reservation and mapping-order controls.

## What is already present and what is missing

The current WDDM2 winsys has virtual BO reservation, buffer_virtual_bind and
UpdateGpuVirtualAddress map/unmap operations. Unmap requests Protection.Zero.
These functions are groundwork, not an end-to-end sparse acceptance result.
Microsoft describes tiled-resource GPU mappings and their residency ownership
in [Tile resources](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/tile-resources).
The local copy is workspace `ref/windows-driver-docs/windows-driver-docs-pr/display/tile-resources.md`,
staging at `110f60eaf2ac5836e644d320c1e92c1011f2af5e`.

That guide also constrains queued mapping implementation. A paging companion
context waits on the UpdateGpuVirtualAddress monitored fence, updates mappings,
then signals completion. Tile-pool physical locations can change while that
work waits. CopyPageTableEntries therefore resolves the current mappings at
execution through the privileged process address space; capturing physical
page numbers at construction is not an equivalent general implementation.
For CPU_VIRTUAL page-table updates, Microsoft describes a separate deferred
UpdatePageTable path without CopyPageTableEntries and without rendering overlap.
Select and implement the applicable mode explicitly. This requirement concerns
tiled-resource mapping and does not by itself invalidate ordinary transfer captures.

The concrete migration failure is M414's compiler assertion in
ac_nir_fixup_smem_loads_null_prt.c. Current AMD compiler information marks this
ASIC as affected by scalar-memory reads from NULL PRT pages. Mesa's workaround
uses two related virtual address ranges. Sparse buffers have a normal HIGH
view and a LOW view; missing LOW regions map a real zero-filled buffer. Scalar
loads clear a reserved address bit and use the LOW view. Normal allocations
must respect that address-bit policy; mappings of resident pages must keep
both views consistent. The WDDM2 port does not supply that policy or
address_prt_wa_control_bit. Assigning an arbitrary nonzero bit would only hide
the assertion, not establish correct mappings.

Accordingly radv_sparse_enabled returns false when the workaround is required
and the bit is absent. This deliberately gates the current sparse feature set;
it is not proof that basic sparseBinding, buffer residency and image residency
all have identical requirements. Evaluate them separately during implementation.

KMD149 now encodes native Zero/PRT and advertises ZeroInPteSupported.
M484 verifies initial/unmapped zero reads and discarded writes through CP DMA,
with exact bound-data controls. Four-level terminal encoding is host-tested;
higher-level hardware walks, shaders and images remain unverified. The separate
COPY_DATA access path still triggers TDR in the observed hole test. Native
Zero support does not complete the scalar alias or queued mapping requirements.

Local primary references: ref/ddi-display/d3dukmdt.md, DXGK_PTE,
D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE and
D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION; ref/ddi-display/d3dkmddi.md,
DXGK_GPUMMUCAPS. The consolidated sources include exact WDK26100 declarations.
Code review and source hashes are in
[the source review](../../evidence/windows/2026-09-24-E27-m9-recovery/sparse-uma-review/RESULT.md).

## Implementation and acceptance

1. Specify VA ranges/control bit with WDDM reservation ownership, address32_hi,
   replay/capture addresses and the KMD VA width. Port the existing AMD/Linux
   algorithm, adapting the OS mapping API rather than inventing register values.
2. Implement matching HIGH/LOW reservation, initial zero mapping, bind/unbind
   and destruction. Keep both views ordered with the queue's real VM fences;
   retain backing until their users retire. Establish the Zero/PRT or OS-emulated
   page semantics on this unit before enabling capabilities.
3. Test resident/unbound/rebound pages with distinct known values, scalar and
   vector reads, aliases and writes into holes followed by reads. Follow the
   exact advertised Vulkan guarantees: strict nonresident behavior means zero
   reads and discarded writes; without it safe undefined reads are still
   distinct from a fault or hang. Basic sparseBinding requires complete binding
   before access; sparseResidency is a separate feature.
   [Vulkan sparse resources](https://docs.vulkan.org/spec/latest/chapters/sparsemem.html).
4. Cover buffers first, then image block layouts, mip tails and advertised image
   formats/sample counts; include aliasing and queued remaps. Compare the chosen
   CTS sparse cases with Linux on this unit at the same Mesa revision. Re-run
   ordinary M8/M9 content controls; turn on only the features actually accepted.

This work is not performed merely by upgrading Mesa, adding SDMA INDIRECT,
or enabling a capability flag. It does not require implementing O2 adaptive
CPU/GPU execution. Sparse hardware acceptance remains open.

## Implemented groundwork
The current Mesa05e6c962 candidate fixes virtual reservation ownership (M481)
and batches mapping requests behind application waits and previous rendering
(M482). A GPU wait on the OS mapping-completion fence precedes later work and
signals. Host models exercise the actual request-building functions; ordinary
compute/present controls check regressions. Neither is sparse GPU acceptance.
The native page semantics, scalar alias mapping and privileged runtime PTE
resolution described above remain required before enabling capabilities.

## Native mapping control (M483)
Native bound alias/rebind reads pass, but the initial-hole CP read causes
VIDEO_TDR_FAILURE on KMD147. API/fence success alone did not prove Zero semantics.
[Results and limits](../../evidence/windows/2026-09-25-E33-native-sparse/RESULT.md).

M484 [native Zero/PRT evidence](../../evidence/windows/2026-09-25-E33-zero-prt/RESULT.md)
replaces the earlier missing-encoder status. Sparse feature advertisement remains gated.
