# ADR 0008: the full WDDM miniport (M7) arrives behind a gate, in four stages, each an experiment

Date: 2026-09-21. Status: accepted.

## Context

- M6 leaves us with a GPU that runs our packets under Windows, interrupts the CPU, signals fences and (0.6.2)
  runs a compute shader, all inside the display-only miniport and driven through escapes (ADR 0007).
- A Vulkan driver cannot talk to a display-only miniport: no allocations, no contexts, no submission. M8 needs
  the VidMm and VidSch half, which means `DxgkInitialize` instead of `DxgkInitializeDisplayOnlyDriver`.
- One PCI function has one function driver (`docs/research/m7-full-wddm-miniport.md` section 2.6). The display
  path and the render path live in one binary or not at all.
- In the scheduler and paging DDIs a failure return is a bugcheck (same document, section 1.2(d)). The
  project's habit of returning an honest failure code does not carry over to them.
- What Windows does with a started full adapter that has no Direct3D user-mode driver is documented nowhere
  (section 2.3). It has to be measured, on a machine that can be recovered.
- Linux has no working GPU reset for this part (facts M53): `amdgpu_gpu_recover` performs no reset and hangs
  the machine. There is nothing to transcribe for `DxgkDdiResetFromTimeout`.

## Decision

1. **`DriverEntry` becomes a gate.** `EnableFullWddm` under `Services\bc250kmd\Parameters`, default 0, reset to
   0 by every install (ADR 0007 point 2). At 0 the driver calls `DxgkInitializeDisplayOnlyDriver` with today's
   table; at 1 it calls `DxgkInitialize`. The start budget (`UnconfirmedStarts`) counts both paths.
2. **Compile at `DXGKDDI_INTERFACE_VERSION_WDDM2_0`, report `DXGKDDI_WDDMv2`.** The version that introduced
   GpuMmu, with every later member structurally absent. Moving up is a later decision with its own reason.
3. **Full graphics, never `ComputeOnly`, never render-only.** GpuMmu, not IoMmu (8 GB of local memory; and unit
   A has no IOMMU, facts M47). One node (`DXGK_ENGINE_TYPE_3D`) first; SDMA as a copy node later.
4. **The new DDIs live in files of their own** (`wddm.c` for the table and caps, `vidmm.c`, `vidsch.c`), and
   the display and bring-up files move over unchanged (ADR 0006 point 6, ADR 0007 point 6).
5. **The DDIs that must not fail never fail**: `PreemptCommand`, `ResetFromTimeout`, `RestartFromTimeout`,
   `SubmitCommand`, `SubmitCommandVirtual` return `STATUS_SUCCESS` on every path; `BuildPagingBuffer` returns
   one of its three legal codes and consumes nothing for an operation it does not know. What went wrong inside
   them goes to the guard log and to an escape, not into the return value.
6. **Four stages, each an experiment with evidence and an exit criterion** (research document section 5):
   A - the adapter starts and the desktop survives, no submission, run with and without a stub user-mode
   driver; B - segments, GPU VA and `BuildPagingBuffer` onto the PTE format M4 validated, driven by a D3DKMT
   tool and witnessed through `bc250rd`; C - one context, one submission through `SubmitCommandVirtual`, one
   monitored fence reaching 1; D - the Vulkan ICD (M8 begins).
7. **Until a reset exists, timeouts are avoided, not recovered**: bring-up contexts use
   `DisableGpuTimeout`, and `ResetFromTimeout` stops the engines the way the undo path does (CP halt, facts
   M44) and marks the GPU path failed until the next device start. A real reset sequence is research of its
   own (the SMU's and the PSP's messages on this part), not a precondition of stages A to C.
8. **The user-mode contract is a versioned private blob** (`driver/contract/bc250_umd_private.h`), checked on
   the host against Mesa's unmodified `ac_gpu_info.c` and against what RADV printed on this unit (facts M50).
   Values the kernel replaces on Linux are replaced here too: `gb_addr_config` is amdgpu's golden constant
   `0x00100044`, not the register.

## Consequences

- A closed gate gives the owner the driver that runs the display today, byte for byte in behaviour. Every
  full-WDDM start is an act of an experiment.
- Stage A can cost a black desktop. The gate, the start budget, `ErrorControl = 0`, the recovery boot entry and
  the lab's freedom to be rebooted are what it relies on; the owner's own PC is never the target.
- The display path changes once, in stage A: `PresentDisplayOnly` has no place in the full table and is
  replaced by `Present` plus `SetVidPnSourceAddress` onto the inherited framebuffer.
- RADV on this part uses the gfx ring only (facts M50), so one 3D node covers M8's first needs.
- A TDR during M7 means a device restart at best and the owner's button at worst. That is accepted for the
  lab and is the reason the stages before C submit nothing.

## State and deviations (2026-09-22)

Stages A, B and C have met their exit criteria: facts M71 (E16 run 009), M73 (E18 runs 002 and 003), M77 (E19 run
005; its limit - a buffer of NOPs - is E19's H4). Where the work left the plan above, and why:

- Decision 4 names `vidsch.c`. There is none: the scheduler-facing half of a submission is a page of `wddm.c`
  (`WddmSubmitHardware`, `WddmGpuFence`, the watchdog), and the ring half lives in `gfx.c` next to the ring it
  writes (`GfxSubmitIb`), under the lock the bring-up escapes already take. One in flight at most.
- Decision 6 C says "one monitored fence reaching 1". At the interface version of decision 2 the driver has no way
  to signal a monitored fence itself: `DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED` exists from WDDM 2.2 on
  (`d3dkmddi.h:712-713`, WDK 10.0.26100.0). dxgkrnl signals it after our `DMA_COMPLETED`, which is what E19 run 005
  shows.
- Decision 7 asked for `ResetFromTimeout` to halt the CP. What exists instead is a 500 ms watchdog in `wddm.c` that
  completes the fence in software and closes the ring path for the device start (`GfxSubmitFail`), so that the
  scheduler never sees a timeout it cannot win; nothing is halted. Never exercised on the hardware so far.
  `ResetFromTimeout` itself still only logs (0.7.13): it does not call `GfxSubmitFail`, so a TDR with a packet in
  flight would leave the ring path open. A gap, to be closed in the next driver build.
- Stage C's design brief planned a registry gate of its own for the IB escape. The WDDM path has one
  (`EnableGpuSubmit`, default 0, closed by every install); the escape modes (`fence gfx 1 ib`, `ib`) are gated like
  every other fence escape - `EnableGfx`, the bring-up state and an administrator - and by nothing else (review 13).
- Lab procedure that the stages taught and the ADR did not foresee: no install over a running full table (facts
  M74); a bring-up run starts from a fresh boot and ends with the complete undo (facts M78).
