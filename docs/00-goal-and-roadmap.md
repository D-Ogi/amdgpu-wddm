# Goal and roadmap

## Goal

A Windows driver that makes the BC-250's GPU do real work: first provably execute commands, then compute, then 3D. "Works" always means demonstrated on hardware with evidence in `evidence/`, never "the code path exists".

Non-goals: modifying AMD firmware, flashing BIOS as a requirement, shipping binaries of unknown origin, supporting hardware other than PCI `1002:13FE` until it works there, production driver signing (the driver is test-signed and the machine runs with test signing on; Microsoft's attestation signing needs an EV certificate and a partner account, which is an organisational matter and stays outside the project by the owner's decision of 2026-09-21).

## Why this is believed possible

Linux drives the same silicon with an open, MIT-licensed kernel driver, from the same BIOS-provided state Windows inherits. No evidence of a Windows-specific lock exists; the two "firmware refuses" results in prior art were both wrong addresses (`prior-art.md`). What is genuinely missing on Windows is large but ordinary: a WDDM miniport that performs the `amdgpu` bring-up, and a user-mode driver on top.

## Milestones

Each milestone has an exit criterion that is a measurement. A milestone is closed by a commit that adds the evidence and updates `facts.md`.

| # | Milestone | Exit criterion |
|---|---|---|
| M0 | **Linux baseline of our unit** (`tools/diagusb`, experiment E01) | `evidence/linux/` holds a full run: pre-driver raw register values, amdgpu dmesg, IP discovery, firmware versions, kernel-side register values. `facts.md` records whether regcalc offsets return the known stock values |
| M1 | **Same reads under Windows** (E02) | A minimal Windows kernel driver (or the predecessor's peek tool) reads the E01 register list through BAR5 and the stable registers match the Linux pre-driver values 1:1 |
| M2 | **Dev loop** | WDK build, test signing, kernel debugger attached to the BC-250 (network or USB3 debug, to be determined), driver install/uninstall scripted, crash dumps collected |
| M3 | **WDDM miniport skeleton that owns the device** | Our miniport binds to `1002:13FE`, keeps the firmware-provided display alive (post-display ownership, single mode), survives start/stop/reboot. No engine work yet |
| M4 | **Memory: GMC + GART + VM context 0** | Page table programmed as `gmc_v10_0` does; the mmhub registers read back like the Linux baseline; no fault status bits set |
| M5 | **First command execution** | Firmware loaded the way Linux does for this ASIC, RLC and CP running, the `gfx_v10_0_ring_test_ring` equivalent passes: a PM4 packet written by the CPU changes `SCRATCH_REG0`. Then the SDMA ring test |
| M6 | **Interrupts, fences, compute queue** | IH ring delivers EOP interrupts; a compute queue mapped through MQD/HQD runs a dispatch that writes a known pattern to memory |
| M7 | **WDDM scheduling and memory management** | VidMm/VidSch contracts implemented for real: GPU VA, paging through SDMA, TDR recovery |
| M8 | **User mode: a Vulkan device** | RADV with a WDDM winsys (ADR 0005) against `driver/contract/`, loaded as an ICD on unit A under Windows: `vulkaninfo` lists the part from our ICD, and the eight compute tests of E14, run from the same SPIR-V, give hashes equal to the CPU's and to the Linux run's (facts M50); the deliberately wrong shader is caught. Submissions go through dxgkrnl and M7's scheduler, not through an escape |
| M9 | **Compute that somebody would use** | llama.cpp on its Vulkan backend under Windows: greedy text from stories15M Q4_0 and TinyLlama 1.1B Q4_0 hashes equal to the GPU hashes measured under Linux (facts M51), identical across at least four runs each. Throughput (pp512, tg128) measured at a stated shader clock and set against M51/M52; a gap is explained in `facts.md` before the milestone closes, not after. This is the first milestone that needs memory beyond a few buffers: allocation, eviction and paging under a working set of about 1 GB |
| M10 | **A picture from Vulkan** | Win32 WSI: `vkcube` (or an equal test with a defined image) shows on unit A's screen, witnessed by `bc250mon` screenshots, frame rate measured. A present path that copies through the CPU is acceptable and is named as such. Whether the software VSync of M7 stage A gives way to the display engine's interrupt is decided by an ADR inside this milestone (ADR 0005 point 4 leaves the display core open) |
| M11 | **Robustness** | 24 hours of mixed load on unit A (M8's compute tests, M9's inference, M10's presentation in rotation) with no TDR, no bugcheck, no live kernel report, no leak in our pool tags and hashes equal at the end. A deliberately stuck queue is run once and what happens is recorded: this part has no working reset under Linux either (facts M53), so the criterion is a known, documented outcome, not a recovery that may not exist. Temperature inside `docs/hardware.md` throughout |
| M12 | **Full user mode for applications** | Every API an application can bring with it works on unit A through our Vulkan driver, each measured against the same unit under Linux with the same Mesa version. (1) Vulkan: the ICD is registered system-wide, and the Vulkan CTS must-pass list gives the same pass set as RADV under Linux; every difference is a `facts.md` row with a cause, none is "flaky". (2) Performance: a fixed benchmark set, written down when M12 starts (compute from M8/M9 plus rendering), within 10 % of Linux at the same shader clock, or the gap explained by measurement. (3) OpenGL 4.6 through Zink: `piglit` quick profile with the pass set of Zink on RADV under Linux. (4) OpenCL through a layer on our Vulkan (clvk or rusticl, chosen by an ADR): `clinfo` lists the device and the layer's own conformance subset passes as it does under Linux. (5) Direct3D 9 to 12 through DXVK and vkd3d-proton: a written list of real applications and benchmarks (at least one per API generation) runs a full pass without a hang, images against a reference, frame rates against Proton on the same unit. Hardware video is outside M12, and not for lack of silicon: IP discovery lists hardware id 12 (UVD/VCN) on unit A (E01, `ip-discovery-sysfs.txt`), but amdgpu deliberately adds no driver block for this family's VCN 2.0.3 (`amdgpu_discovery.c`: `case IP_VERSION(2, 0, 3): break;`) and offers no UVD, VCE or VCN ring under Linux (facts M46), so there is no open bring-up to import (rule 7) and no reference to measure against. The desktop itself still composes on WARP until M13 |
| M13 | **Native Direct3D user-mode driver: an accelerated desktop** | ADR 0009 (supersedes ADR 0005 point 2). `D3D11CreateDevice` on the hardware adapter succeeds without any DLL next to the application (E16's `-Phase d3d` is the probe, its HRESULT today is the baseline), DWM composes on our adapter instead of the Basic Render Driver, and the desktop survives M11's 24 hours on it. The route (Mesa's `d3d10umd` frontend over Zink on our Vulkan, or over a gallium driver with a WDDM winsys, or something else) is chosen by measurement when M12 closes, and ADR 0009 records the choice |
| O1 | Optional: 40 CU | Only after M5. Mirrors `bc250-40cu-unlock`: two per-bank register writes during gfx init |

Status 2026-09-21: M0-M5 closed on unit A (facts M1-M33; M3 by experiment E06: our display-only miniport runs the
lab machine's display, and `D3DKMTEscape` reaches it, which gives M4-M6 their control channel). M2's debugger
works over KDNET but its host-side server is disabled after it hung the development PC (journal 2026-09-21).
M4 by experiments E07-E09: AMD's hub code, imported unmodified and run inside the miniport, programs GART and VM
context 0 with the same register writes amdgpu made on this unit; acknowledged by the hardware, no fault bit,
picture undisturbed, reversible. The driver does it on command behind gates; doing it at every start comes
with M5, whose ring buffer is the first thing that needs a GART mapping. M5 is under way: its first part, the
firmware through the PSP, works under Windows (E10, facts M34, M35: AMD's `psp_v11_0_8.c` imported unmodified,
eleven commands accepted, same register traffic and timing as amdgpu on this unit; the PSP itself starts the RLC
and releases SDMA). M5 is reached (E11, facts M36, M37): amdgpu's GFX and SDMA bring-up for this part, transcribed
against AMD's unmodified tables and run in stages inside the miniport, makes the same 355 register writes amdgpu
made (14 of them carrying our addresses), and all eleven ring tests pass with the rings in system memory behind
our GART. M6 in progress: Windows assigns the miniport a message interrupt and it is silent with no source enabled (E12 part A, facts M38); a second bring-up in the same boot works (M39). The IH ring runs and the GPU interrupts the CPU: fences on the gfx ring, the compute queues and the KIQ each give one vector, the same as under Linux on this machine (E12 run 002, E13, facts M40, M43). M6 is reached (E15, facts M57, M58): a compute dispatch on a kernel-owned MEC queue writes its pattern under Windows, the SDMA ring tests and fences work, and a second bring-up in one device start no longer meets a live KIQ fetcher. One defect found on the way: the SDMA engines keep their ring pointers across a halt, so a re-init has to adopt them (M59); 0.7.0 does, and three bring-ups in one device start now work end to end (E15 run 002, M60). M7 starts from ADR 0008. Its stage A met dxgkrnl for the first time in E16 run 001: the full table starts, is refused before the first VidPN and unloaded without a reason on record, and the display-only driver comes back unharmed (M61, M62).

M8's criterion was set and M9-M13 were written out on 2026-09-21, at the owner's request, before any of them was
started. The order is the order of dependence: M9 needs M8's device, M12 needs M10's swapchain, M11 needs something
worth soaking, M13 needs M12's Vulkan to stand on. A criterion may be sharpened by an ADR when the milestone before
it closes; it is never loosened after a run.

Sizing: this file used to say "M0-M1 days, M2-M3 weeks, M4-M6 the real research, M7-M8 months to years". M0 to M6
then closed within the repository's first day of commits (99 of them), so calendar estimates are dropped: the first
one was wrong by two orders of magnitude and a second one would be no better founded. What can be said is relative.
M7 is the largest kernel-mode step left, M8 the largest user-mode one, M9 to M11 are tests of what M7 and M8 built,
M12 is breadth, and M13 is the one milestone whose route is not known yet. The project stays useful at every step because each milestone leaves verified, published knowledge.

## Architecture direction (detail in ADRs)

- One WDDM miniport owns the PCI function (GPU and display are the same function, so a display-only driver cannot coexist with a render driver).
- Hardware bring-up code is imported from `amdgpu` and compiled against a thin compatibility shim (`driver/shim`) rather than rewritten, the way the BSDs port DRM drivers. Register headers and `SOC15_REG_OFFSET` are used unchanged.
- Every MMIO path that has not been proven on hardware sits behind a gate that defaults to off.
