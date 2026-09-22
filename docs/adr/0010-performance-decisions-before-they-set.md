# ADR 0010: the decisions that decide performance, written down before they set

Date: 2026-09-22. Status: **accepted** (the owner asked, after stage C, whether the architecture should change while
it still can; this is the answer, and the owner accepted it the same day).

## Context

The goal is games on Vulkan at a frame rate worth having. Stage C of ADR 0008 has just shown the whole spine working
(facts M77, M80). Some choices are cheap to revisit later and some are not, and the owner's question was which is
which - including whether C is the right language, or whether Rust or hand-written assembly belong here.

## Decision

1. **The kernel driver stays C.** A frame's cost is not spent in the miniport: it is the control path (memory, page
   tables, a pointer onto a ring, an interrupt), and the GPU does the work on command buffers a user-mode driver
   built. C costs nothing against Rust there, assembly buys nothing in control code, and the copies that are hot go
   through `RtlCopyMemory`, which already is assembly. The WDDM headers and the imported amdgpu code are C (hard rule
   7: import, do not retype). Rust's real offer is memory safety, not speed; it is declined for now on cost, not on
   merit, and nothing in this ADR stops a self-contained module from being written in it later.
2. **Kept, because it is the one choice that could not be undone:** GpuMmu, `NoPatchingRequired`, page tables in
   the hardware's own format, and the user-mode command buffer executed in place as an indirect buffer (facts M73,
   M80). No copy, no patching and no per-submission validation in the kernel. This is amdgpu's model.
3. **The CPU blit of E20 is a diagnostic and stays behind a gate.** The present path for games is a flip: the
   scanout moved to the presented surface (DCN, `SetVidPnSourceAddress`) and a real vertical-sync interrupt in
   place of the timer. It needs a Linux trace of one flip first (`docs/linux-session-wishlist.md`).
4. **Submission is rebuilt before the ICD is tuned on top of it:** a ring allocator that honours the read pointer
   and keeps many submissions in flight, a VMID held per process instead of re-pointed per packet, the VM flush on
   the ring instead of by MMIO under a mutex, a fence per submission. Internal to `gfx.c` and `wddm.c`, hence
   reversible - but everything measured above it is wrong until it is done.
5. **The node layout is fixed before the user-mode contract is:** node 0 3D, node 1 copy and paging on SDMA, so
   that paging never queues behind rendering. User mode sees the nodes, which makes this the sticky one. SDMA's seven
   known defects (`driver/shim/test/sdma_faults.c`) are closed first.
6. **Clocks are on the map early.** The GPU runs at a fixed, low operating point set by a startup task. Power
   management through the SMU will move the frame rate more than any line of driver code; it blocks nothing in the
   architecture and is scheduled rather than designed here.

## Consequences

- Points 3 to 6 are each a decision of their own and are carried by ADR 0011 (present is a flip), ADR 0012
  (submission), ADR 0013 (node layout) and ADR 0014 (clocks); this ADR keeps points 1 and 2 and the ordering.
- Nothing already built is thrown away. Points 3 to 5 are work ahead, in that order of stickiness: 5, 3, 4.
- Until point 4 is done, no performance number from this driver means anything, and none is to be recorded as a fact.
- If the owner prefers Rust for new modules regardless, point 1 is the only one that changes, and the cost is the
  bindings for `d3dkmddi.h` and a build for them in `driver/kmd/build.ps1`.
