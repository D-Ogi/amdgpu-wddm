# Privileged virtual PTE copies

The companion paging context may wait while Windows relocates a tile pool.
Microsoft's local Tile resources guide requires the queued copy to use the
current mapping at execution. A command containing build-time physical addresses
cannot provide that guarantee.

The existing physical copy path remains deployed in KMD149. The new
bc250_sdma_build_virtual_ptes builder prepares a replacement IB: source and
destination remain GPU virtual addresses, and an intermediate page provides
snapshot semantics even when two different VAs alias overlapping physical bytes.
CSA, marker, commands and staging occupy disjoint portions of the retained OS
DMA span. No allocation, CPU table walk, root choice or submission occurs here.

The packet emitters are the existing AMD-derived copy and fence/poll helpers.
PROVENANCE: Linux amdgpu, MIT, v6.18 at 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
WDDM ownership is a separate caller responsibility. The new module is not wired
into BuildPagingBuffer, linked into the deployed KMD or hardware-verified.

## Controls

Run from the repository root:

    powershell -NoProfile -File driver/shim/test/run_paging.ps1 -VirtualPtes
    powershell -NoProfile -File experiments/E33-m12-applications/compile_virtual_ptes_kernel.ps1

The host test builds the actual C emitters and interprets their COPY, FENCE,
POLL and NOP packets. It changes both endpoint mappings after construction,
then compares all data bytes with an independent memmove oracle. It covers
distinct pages, self-copy and both overlap directions through distinct VAs.
Old backing must remain untouched; a stale-source counterfactual must differ.
All 1024 DWORD-aligned starts within a page check the embedded storage layout.
A one-byte-short DMA span must request another buffer without publishing work.

M488: 148 relocation/alias cases, 1024 alignments, 15048 total checks including
10064 existing packet checks, zero failures. WDK26100 kernel compilation passes.
These are host controls, not GPU or Windows scheduler evidence.

## Integration and lab procedure

1. Route CopyPageTableEntries through the privileged context's GPU VA IB.
   Preserve whole-range MultipassOffset progress and exact OS DMA/private
   buffer ownership. The embedded staging page can require more than a 4 KiB
   context DMA buffer; size the companion context accordingly.
2. Obtain the privileged root at submission after SetRootPageTable. Existing
   native records capture the pinned system root during construction; that
   assumption needs separate treatment for a movable privileged process root.
   Retain the selected root through real completion, including software queuing.
3. Keep construction shadows for registered pinned paging tables coherent.
   Application/privileged tables do not necessarily have such a shadow.
   Do not require their physical addresses merely to build the GPU commands.
4. Before enabling the DDI path, extend the owned VMID2 control: distinct
   source/destination patterns, a GPU-ordered mapping change before the same
   already-built IB, full readback, and overlapping table ranges. Check the
   old backing as well as the new destination. Stop on timeout or mismatch.
5. Exercise actual Windows companion operations with delayed waits and memory
   relocation, then repeat release CTS buffer/image controls. Record native
   route counters, loaded binaries and faults so a fallback cannot pass unseen.
6. Compare the selected sparse CTS corpus with the same Mesa revision on unit A
   under Linux. Broader formats, mip tails and the full required corpus remain.

References: local ref/windows-driver-docs/windows-driver-docs-pr/display/
tile-resources.md at 110f60eaf2ac5836e644d320c1e92c1011f2af5e; local
ref/ddi-display/d3dkmddi.md (WDK26100), DXGK_BUILDPAGINGBUFFER_COPY_RANGE,
DXGKARG_BUILDPAGINGBUFFER and DXGKARG_SUBMITCOMMANDVIRTUAL.
