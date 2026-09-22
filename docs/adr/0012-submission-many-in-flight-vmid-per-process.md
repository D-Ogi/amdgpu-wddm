# ADR 0012: submission - many in flight, a VMID per process, the flush on the ring

Date: 2026-09-22. Status: **accepted** (split out of ADR 0010 point 4 at the owner's suggestion).

## Context

Stage C (facts M77, M80) submits the way a bring-up should: one submission in flight, every context at VMID 1, the
page directory root re-pointed by MMIO with a TLB flush before each packet, all of it under `GartLock` at
PASSIVE_LEVEL, a 500 ms watchdog, ring space rounded up generously. It is correct and it is slow by construction:
the CPU waits for the GPU between any two packets, which is the opposite of what a GPU queue is for.

## Decision

1. The ring gets an allocator that honours the read pointer and wraps, and submissions queue behind one another;
   each carries its own fence sequence, and the EOP interrupt (facts M79) completes every sequence it has passed.
2. VMIDs 1 to 15 are held per process (per root page table) and reassigned least-recently-used, as amdgpu's
   `amdgpu_vmid_grab` does; a context that keeps its VMID and root pays no flush at all.
3. When a VMID does change hands, the root and the flush go down the ring in front of the indirect buffer
   (gfx_v10's `emit_vm_flush`, imported through the shim per hard rule 7), not by MMIO from the CPU.
4. Submission moves off the mutex: what `SubmitCommandVirtual` does per packet must be legal and cheap at
   DISPATCH_LEVEL.
5. The watchdog becomes dxgkrnl's TDR (`ResetFromTimeout`) plus a per-ring hang check, not a timer per packet.

## Consequences

- Internal to `gfx.c`, `wddm.c` and the shim; user mode sees none of it. Reversible, and for that reason scheduled
  after the things that are not (ADR 0013), but before any performance work above it.
- Needs from Linux: the PM4 stream of a real VM flush and how VMIDs move between processes (wishlist).
- The escape-driven engines (E17, E19 controls) keep working: they submit through the same allocator.
