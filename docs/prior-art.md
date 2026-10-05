# Prior art: Windows driver attempts for the BC-250

Surveyed 2026-09-21. Local read-only clones live in `<BC250_ROOT>\ref\` (`BC250_ROOT` is the workspace root, by default the parent directory of this repository). None of these sources is a source of facts for this project (see `01-evidence-rules.md`); they are sources of ideas, of verified code we may borrow where the license allows, and of mistakes not to repeat.

Community baseline: `lildebil0/awesome-bc250`, `docs/en/07-windows.md` (state "early 2026"). Its summary is accurate: every official AMD driver ends in Code 43 because no shipping driver knows PCI `1002:13FE`; Linux works because `amdgpu`/Mesa are open and were patched; the from-scratch Windows efforts are at the "can we initialize the GPU at all" stage. Nothing in it points to a hardware lock against Windows. It also records the social context worth remembering: a history of hoaxes, malware posted as "drivers", and an unverified "leaking drivers bricks boards" rumor. Consequence for us: publish source and evidence, never binaries of unknown origin.

**The record, as of 2026-10-05.** Three community documentation projects state plainly that no Windows GPU
driver exists for this board: `elektricM/amd-bc250-docs` at `954b706` (`docs/drivers/radv.md`,
"No Windows drivers exist"), `katzzero/bc250-unofficial-community-guide` at `ee8df94` and
`kalpakprod/awesome-bc250` at `d18d3ce` (`docs/en/07-windows.md`). That is no longer so. This driver renders
the Windows desktop on the GPU and runs DirectX 12 games on unit A. Our facts graph carries the measurements.
Two limits hold on that statement. The three earlier attempts above are still at the stage this page describes.
Our own driver is at tester quality on one unit, and it is not a product. PROVENANCE: we cite the three
projects for their statements only. Their licences are MIT (`katzzero`) and dual documents-and-code
(`elektricM`, `kalpakprod`). We copied no code and no text.

## 1. Keshas-dev/AMD-BC-250-Windows-Driver

Clone `ref/keshas-driver__WARN-AGENTS-md-is-not-facts` @ `63f8956` (2026-09-16). Apache-2.0. Tested on one real unit (BIOS P4.00G / `BC250_5.00_clv.bin`).

- What it is: a WDM IOCTL driver plus a KMDOD-based display driver, a stub Vulkan ICD, many user-mode test tools. Not a WDDM render miniport.
- Verified-looking parts worth borrowing: SMU mailbox over SMN (clocks, voltages, telemetry, CPU core unlock) with allow-lists and voltage limits; PSP GPCOM ring and `LOAD_IP_FW` once the MP0 base was corrected to `0x58000`; display at 2560x1440 through KMDOD; init-step kill switches and a last-step marker that survives a reboot.
- Why it stalled: GC register offsets computed as `0x1260 + mm*4` instead of `(0x1260 + mm) * 4`; seg1 registers without the `0xA000` base at all. Every "SOS-locked / NBIO-locked" finding was measured at those offsets. Full analysis: `predecessor-analysis.md`.
- Its `AGENTS.md` (160 KB of agent memory) contains mutually contradictory "REAL" facts. Do not feed it to an AI agent as context.

## 2. Keshas-dev/AMD-BC-250-PSP-Driver

Clone `ref/AMD-BC-250-PSP-Driver` @ `3bfa7a2` (2026-07-21). MIT. About 800 lines of C plus scripts; the older companion of (1).

- What it is: a kernel driver talking to the PSP through C2PMSG mailboxes, acting as a register proxy for the GPU driver. Includes BIOS/PSP-directory parsing scripts and UEFI variable dumps, which are useful reference material for `docs/hardware.md`.
- Its headline negative result, "GPCOM/TOS ring protocol: SOS doesn't support ring-based commands", was measured with `MP0 base = 0x16000` used as a byte offset (`inc/PspIoctl.h`). The correct byte base is `0x16000 * 4 = 0x58000`, and with that base the same author later got the ring working in repo (1). This is the same class of error as the GC addressing bug, and the clearest demonstration in the wild that on this chip "the firmware refuses" has twice meant "wrong address".
- "KIQ_BASE/KIQ_SIZE hardwired to 0": those registers do not exist in GC 10.1 (`regcalc.py lookup` says NOT FOUND).

## 3. ZEROAESQUERDA/BC250-windowsDriverTest

Clone `ref/BC250-windowsDriverTest__WARN-no-licence-read-only-copy-nothing` @ `71c1f01` (2026-08-20). **No license file: all rights reserved, we may read it but not copy code.** About 5300 lines.

- What it is: the only attempt shaped like the real thing, a full WDDM miniport skeleton (`DxgkInitialize` with `DRIVER_INITIALIZATION_DATA`: CreateDevice/Context/Allocation, Render, Patch, BuildPagingBuffer, SubmitCommand, fences, preemption, TDR) plus a UMD boundary DLL. By its own account it was written without access to a BC-250 and was never compiled with the WDK ("sandbox Linux"). Treat it as a design sketch.
- What it gets right, and we adopt as practice: honest state machine for firmware (`Present / Valid / Loaded / Ready`), every unproven MMIO path behind a compile-time gate that defaults to off (`BC250_GFX_OFFSETS_VALIDATED=0`, interrupt and PSP gates likewise), DDIs returning `STATUS_NOT_SUPPORTED` / `E_NOTIMPL` instead of faking success, a GPU-written fence (`WRITE_DATA` with `DST_SEL=5 | WR_CONFIRM`) instead of a CPU-written one, UMA memory model that does not invent local VRAM.
- What it gets wrong:
  - Register offsets are `mm * 4` with **no IP segment base**: `CP_RB0_BASE` is placed at `0x7780`; the correct offset is `(0x1260 + 0x1DE0) * 4 = 0xC100`. The opposite half of Keshas' mistake. The author suspected it ("Cyan Skillfish2 has a distinct IP offset table") and kept the gate closed, which is exactly what gates are for. The table they were missing is `cyan_skillfish_ip_offset.h`.
  - It treats BAR5 as "the SMN mailbox BAR" and expects GC/SDMA registers in a different BAR. On this ASIC all of it is BAR5 (`amdgpu` maps `pci_resource(5)` as `rmmio`).
  - Its PSP offsets (`0x58200` for C2PMSG_64) are correct, inherited from Keshas' corrected base.
  - No GART/GPUVM, no page tables, ring base taken as a physical address: same gap as (1).
- Useful as: a checklist of the WDDM DDIs a render miniport must provide and of the honest failure codes for the ones not implemented yet.

## What the three attempts have in common

| | Keshas GPU | Keshas PSP | ZEROAESQUERDA |
|---|---|---|---|
| Register addressing | base not scaled | base not scaled | base missing |
| Positive control against Linux on the same unit | never | never | no hardware |
| GART / GPUVM bring-up | disabled (BSOD) | n/a | absent |
| Ring base programmed with | physical address | n/a | physical address |
| Developed largely by AI agents | yes | yes | yes |
| Gates on unverified MMIO | kill switches after crashes | no | yes, by default |

Nobody has yet done the boring first step: read a handful of registers at correctly derived offsets and compare them with the values Linux reports on the same board. That is what `tools/diagusb` and experiment E01 are for.

## Licensing summary

| Source | License | May we copy code? |
|---|---|---|
| Linux `amdgpu` (headers, IP-block code by AMD) | MIT | yes, keep the notices |
| Keshas-dev GPU driver | Apache-2.0 | yes, with attribution in `THIRD-PARTY.md` |
| Keshas-dev PSP driver | MIT | yes |
| ZEROAESQUERDA | none | **no**: ideas only |
| AMD firmware blobs (`linux-firmware`) | AMD redistributable-binary license | never commit; load from files at runtime |
