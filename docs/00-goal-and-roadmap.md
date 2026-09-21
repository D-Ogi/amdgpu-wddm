# Goal and roadmap

## Goal

A Windows driver that makes the BC-250's GPU do real work: first provably execute commands, then compute, then 3D. "Works" always means demonstrated on hardware with evidence in `evidence/`, never "the code path exists".

Non-goals: modifying AMD firmware, flashing BIOS as a requirement, shipping binaries of unknown origin, supporting hardware other than PCI `1002:13FE` until it works there.

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
| M8 | **User mode** | Direction: Vulkan first (RADV with a WDDM winsys), Direct3D by translation (ADR 0005). Exit criterion set at M8 |
| O1 | Optional: 40 CU | Only after M5. Mirrors `bc250-40cu-unlock`: two per-bank register writes during gfx init |

Status 2026-09-21: M0-M4 closed on unit A (facts M1-M33; M3 by experiment E06: our display-only miniport runs the
lab machine's display, and `D3DKMTEscape` reaches it, which gives M4-M6 their control channel). M2's debugger
works over KDNET but its host-side server is disabled after it hung the development PC (journal 2026-09-21).
M4 by experiments E07-E09: AMD's hub code, imported unmodified and run inside the miniport, programs GART and VM
context 0 with the same register writes amdgpu made on this unit; acknowledged by the hardware, no fault bit,
picture undisturbed, reversible. The driver does it on command behind gates; doing it at every start comes
with M5, whose ring buffer is the first thing that needs a GART mapping. M5 is under way: its first part, the
firmware through the PSP, works under Windows (E10, facts M34, M35: AMD's `psp_v11_0_8.c` imported unmodified,
eleven commands accepted, same register traffic and timing as amdgpu on this unit; the PSP itself starts the RLC
and releases SDMA). Next: M5's second part, RLC and CP bring-up, KIQ, the ring tests, SDMA.

Honest sizing: M0-M1 days, M2-M3 weeks, M4-M6 the real research, M7-M8 months to years. The project stays useful at every step because each milestone leaves verified, published knowledge.

## Architecture direction (detail in ADRs)

- One WDDM miniport owns the PCI function (GPU and display are the same function, so a display-only driver cannot coexist with a render driver).
- Hardware bring-up code is imported from `amdgpu` and compiled against a thin compatibility shim (`driver/shim`) rather than rewritten, the way the BSDs port DRM drivers. Register headers and `SOC15_REG_OFFSET` are used unchanged.
- Every MMIO path that has not been proven on hardware sits behind a gate that defaults to off.
