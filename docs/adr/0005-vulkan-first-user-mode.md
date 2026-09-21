# ADR 0005: Vulkan first in user mode, Direct3D by translation

Date: 2026-09-21. Status: accepted (direction only; the exit criteria that follow from it are M8, M10 and M12 in
`docs/00-goal-and-roadmap.md`, set the same day). Point 2's second sentence ("A native D3D UMD is not a goal") and
the first consequence's "acceptable" are superseded by ADR 0009: the WARP desktop is a stage, M13 ends it.

## Context

The roadmap left the user-mode direction open until M8. The owner asked whether Vulkan is the easier
target than Direct3D. It is, but only in user mode, and the difference matters for how the kernel-mode
driver is shaped, so the direction is recorded now.

- Vulkan: Mesa's RADV already drives this GPU under Linux (GC 10.1.3, Cyan Skillfish) and is MIT
  licensed. What it lacks on Windows is a winsys: buffer allocation, command submission, contexts and
  synchronization on top of the D3DKMT interface instead of libdrm/amdgpu. That is a bounded piece of work
  against an abstraction RADV already has.
- Direct3D 11/12: the user-mode DDIs are very large, and no open implementation for AMD hardware exists to
  start from. Writing one is far beyond this project.
- Direct3D 9-11 and 12 applications can run on a Vulkan driver through DXVK and vkd3d-proton, both of
  which work natively on Windows as DLLs placed next to the application.

## Decision

1. The first user-mode target is a Vulkan ICD: RADV with a new WDDM winsys.
2. Direct3D is reached by translation (DXVK, vkd3d-proton). A native D3D UMD is not a goal.
3. The kernel-mode driver is still a WDDM miniport. Vulkan does not avoid VidMm, VidSch, GPU virtual
   addressing, paging, preemption and TDR: the ICD talks to our KMD through dxgkrnl. The user-mode choice
   saves nothing in the kernel.
4. Display starts with the framebuffer handed over by the firmware (the way the Basic Display driver
   does it), one fixed mode. Porting AMD's display core is a later, separate decision.

## Consequences

- Until a D3D UMD exists, the desktop compositor and everything that uses the system's Direct3D renders
  in software (WARP). Applications shipped with DXVK/vkd3d use the GPU. This is acceptable for a research
  driver and has to be said plainly in the README when the time comes.
- RADV's assumptions about the amdgpu kernel interface (context and BO semantics, syncobj, VA management
  in user space) become requirements for our private KMD-UMD interface. They are to be collected from the
  RADV winsys before the KMD's escape and submission interface is designed.
- License: RADV is MIT and may be combined with our code; our winsys stays under the project license
  unless the licensor decides to offer it upstream (ADR 0004).
- Nothing here is measured. It is a plan; the first fact it needs is E02 (MMIO under Windows at all).
