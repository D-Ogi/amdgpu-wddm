# M418 - Sparse support gaps and adaptive-execution scope

Source review2026-09-24, no new lab workload. PROVENANCE: Mesa upstream and
Collabora lfrb/mesa, MIT. Mesa basef333dd6d1c85297ac41773eaeb9b02f16acf1919
with the accepted M414 port. Local consolidated Microsoft DDI documents carry
WDK26100 declarations; exact source hashes/excerpt ranges are in sources.json.

The WDDM winsys already reserves virtual BOs and implements mapping/unmapping
through UpdateGpuVirtualAddress; unmap asks for Protection.Zero. Current AMD
compiler policy marks GC10.1-class hardware with has_smem_with_null_prt_bug.
The compiler pass requires a mirrored VA control bit and corresponding zero
buffer mapping. The Linux winsys implements that algorithm; the Windows port
has not implemented the matching address-space policy. M414's measured compiler
assertion is documented separately in ../radv-main/RESULT.md. The accepted port
disables sparse when that required bit is absent. No sparse CTS or successful
sparse workload is established by this review.

WddmGpuMmuCaps zero-initializes caps without setting ZeroInPteSupported.
The PTE translator returns EINVAL for a valid entry carrying Zero. Its former
comment described Zero as having only zero as a legal value. Local MS DXGK_PTE
explicitly defines the bit's tiled-resource zero-read semantics, and the MMU
cap applies to all levels. Corrected that comment only; runtime behavior and
the installed128binary are unchanged. Native Zero support versus OS emulation
needs a documented design and positive control; caps0 alone is not evidence
that WDDM forbids sparse resources.

Engineering decision: add M12.1 for sparse mapping/zero semantics, mirrored VA
policy and compiler integration, synchronization and per-feature CTS controls.
WDDM's standard capabilities are distinct from this incomplete implementation.

Add optional O2 for adaptive CPU/SDMA/GPU research, initially known buffer
operations and avoiding copies. Shared GDDR6 does not prove cache-coherent UMA
or profitable CPU fallback. Existing M9 cache/PFN acceptance remains open.
The proposal requires legal ownership/synchronization, measured end-to-end
costs and tail latency against fixed policies. Arbitrary game shader or CPU
logic migration is outside its initial scope. These are plans, not measured
speedups or a promised transparent game optimization.

[Mapped implementation plan](../../../../docs/design/sparse-wddm.md) and
[adaptive research gates](../../../../docs/design/adaptive-cpu-gpu.md).
