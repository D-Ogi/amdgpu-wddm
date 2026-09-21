# Why the previous Windows driver attempt concluded "firmware-locked", and why that is most likely wrong

Subject: `Keshas-dev/AMD-BC-250-Windows-Driver` @ `63f8956` (2026-09-16): `README.md`, `AGENTS.md`, `docs/WGP-UNLOCK-STATUS.md`, `docs/RING-INIT-STATUS.md`, `docs/REGISTER-MAP-BC250.md`, `docs/BC250-LINUX-IP-MAP.md`, `inc/amdbc250_dream_hw.h`, `inc/amdbc250_dream_kmd.h`, `third-party/EFI_Boot/psp/*.nsh`.
Reference: Linux `amdgpu` sources (master, 2026-09-21) and `duggasco/bc250-40cu-unlock`.
Date: 2026-09-21. **Desk analysis only: nothing here has been tested on hardware.** The addressing findings follow from arithmetic and kernel source and are certain; what happens after the addresses are fixed is a hypothesis (see "What this does not show").

## Summary

The same board runs 3D and compute under Linux (24 CU stock, 40 CU with a small patch). The Windows author nevertheless concludes "3D impossible, registers SOS-locked, NBIO locked at EFI boot". The most likely explanation is not a lock but arithmetic: the GC block base `0x1260` is an index of 32-bit words in `amdgpu` and must be multiplied by 4, while the author adds it as bytes. Nearly every "GC register" he reads or writes is therefore a different register or a hole. Writes "do not stick", reads return 0, and this was interpreted as a firmware lock. Seven further errors sit on top of that one.

## 1. Evidence that nothing is locked "for Windows"

From the author's own notes: on the same physical unit, under CachyOS, `dmesg` shows `gfx_0.0.0`, eight compute rings, KIQ and SDMA rings created, SMU initialized, DCN running, `active_cu_number 24`, and `bc250_cc_write_mode: unknown parameter ignored` (so no 40 CU patch was present, and 3D worked anyway). He also writes "Not hardware-fused - Linux amdgpu runs shaders". The BIOS, PSP and SMU of that unit therefore leave the chip in a state from which an ordinary kernel driver brings the GPU up completely.

## 2. Error 1 (critical): register address arithmetic

Correct rule: `byte = (segment_base + mmREG) * 4` (see `02-register-addressing.md`). The author: "GC_BASE = 0x1260, so byte offset = GC_BASE + (mm * 4)"; `BC250-LINUX-IP-MAP.md` even says `0x1260 + reg`. Segment 1 (`0xA000`) is never applied.

| Register | Author's offset | Correct offset | What is at the author's offset |
|---|---|---|---|
| `SPI_PG_ENABLE_STATIC_WGP_MASK` | `0x5C3C` (earlier `0x34FC`) | **`0x935C`** | no GC register |
| `CC_GC_SHADER_ARRAY_CONFIG` | `0x3264`, later `0x9C1C` | **`0x89BC`** | `0x9C1C` = `GCEA_PERFCOUNTER1_CFG` |
| `GRBM_STATUS` | `0x3260` | **`0x8010`** | outside GC |
| `GRBM_GFX_INDEX` (seg 1) | `0x34D0` | **`0x30800`** | outside GC |
| `SCRATCH_REG0` (seg 1) | `0x32D4` | **`0x30100`** | outside GC |
| `CP_ME_CNTL` | `0xC060`, later `0x4A74` | **`0x86D8`** | `0x4A74` = `SDMA0_UTCL1_WATERMK` |
| `CP_RB0_BASE` | `0xDA60` | **`0xC100`** | no GC register |
| `RLC_PG_ALWAYS_ON_WGP_MASK` (seg 1) | `0x3D64` | **`0x3B14C`** | outside GC, reads `0xFFFFFFFF` |

All values in the "correct" column are produced by `tools/regcalc`; none of them occurs anywhere in the predecessor's repository.

The author had the rule in his hands: "REAL, WORKING MP0 C2PMSG base in BAR5 = 0x58000 (= 0x16000 in DWORD units x 4)", and a Linux IP discovery dump headed "bases in DWORD units, multiply by 4 for BAR5 bytes" listing `GC/0 0x1260`, directly under the title "CONFIRMED: all our register bases are CORRECT". The PSP path started working the moment the base was multiplied by 4; the lesson was never applied to GC.

Consequence: the "9 unlock methods, all blocked" are one experiment at one wrong offset, repeated nine ways. The noted discrepancy "Linux stock SPI_PG = 0x07, but our driver read 0x00000000" was the decisive clue and was explained away.

"SOS-locked" is not a term from AMD documentation or the kernel. It began as the author's label for "the write did not stick" and hardened into a mechanism.

## 3. Error 2: wrong goal - the WGP unlock is not a prerequisite for 3D

`SPI_PG_ENABLE_STATIC_WGP_MASK` is `0x7` from the factory (24 CU); the unlock changes it to `0x1F` (40 CU). Linux renders on 24 CU without it. The chain "SPI_PG reads 0 -> zero active WGPs -> no shader can run -> no 3D without unlock" starts from a read at the wrong offset. Weeks went into unlocking something optional instead of into the real gap: command submission.

## 4. Error 3: "debugfs privilege" and the "window before the SOS lock"

`amdgpu_debugfs.c` implements `amdgpu_regs` with `RREG32(*pos >> 2)` and `amdgpu_mm_wreg_mmio_rlc()`: the driver's ordinary MMIO accessors. No extra privilege exists. The 40 CU patch writes in `gfx_v10_0_get_cu_info()` during normal module init, which can happen long after boot, and `bc250-cu-live-manager` flips the registers at runtime with `umr`. There is no early window. The theory survived because it explained everything and excused the driver ("Our drivers are NOT buggy"); the self-audit checked the write function and the BAR mapping, not the offset.

## 5. Error 4: "NBIO locked at EFI boot"

The EFI shell script uses the same wrong offsets (`mm FE805C3C ...`, `FE809C1C`, `FE803D64`, `FE8034D0`), so it is not an independent test. The run that "confirmed" the lock is dated 2026-08-11 and reports C2PMSG registers reading `0xFF`; the script carries the comment "FIX 2026-08-21: mm defaults to 1-BYTE width!". The conclusion predates the fix by ten days and was never withdrawn. `NBIO_ID = 0xFEDCBAEF` at `0xC100` is in fact the content of `CP_RB0_BASE`. And the same unit with the same BIOS works under Linux, so "BIOS NBIO unlock is the only path" does not follow.

## 6. Error 5: registers that do not exist, offsets chosen because they were writable

`KIQ_BASE_LO`, `KIQ_CNTL`, `CP_RING0_BASE_LO`, `COMPUTE_RING0_BASE_LO` do not exist in `gc_10_1_0_offset.h`. In GFX10 the KIQ is an ordinary compute queue programmed through `CP_HQD_*` after an ME/pipe/queue select. The "Navi10 0xC800+ -> BC-250 0xDA60+" map was generated, not transcribed. The criterion for a correct offset became "it accepts a write": `0x34D0` was kept for `GRBM_GFX_INDEX` because "HW reports it as live", `0x4A74` for `CP_ME_CNTL` because a write of 0 stuck (it is an SDMA watermark register). Offsets that did not react were labelled locked. Classic confirmation bias.

## 7. Error 6: the queue model

Even with correct offsets the ring init would fail: it programs the ring's **physical** address, while `gfx_v10_0` programs `ring->gpu_addr >> 8`, a GPU virtual address in the GART; GART and VM init are disabled by registry switches because they blue-screen (`0x1A`); compute/KIQ queues need MQD/HQD programming and `RLC_CP_SCHEDULERS`; banked registers need a working `GRBM_GFX_INDEX`. Linux order is common -> gmc -> ih/psp -> smu -> display -> gfx -> sdma. The driver also reports 16 GB of "VRAM", while Linux sees the BIOS carve-out plus GTT. Notes on firmware loading contradict each other and the kernel (`psp_v11_0_8.c` does implement the ring; for `13FE` the load type is PSP).

The author did list "WDDM miniport - full init order like Linux" - as one of four exotic alternatives rather than as the diagnosis.

## 8. Error 7: success metrics that do not measure the goal

"Vulkan ICD pipeline VK_SUCCESS" comes from a stub ICD that never touches the GPU. "SMU 16/16 PASS" is solid work about clocks and voltages, not graphics. The architecture (WDM IOCTL driver + KMDOD) cannot receive 3D work from Windows at all. A long green list created the impression that the infrastructure works and the hardware must be at fault.

## 9. Error 8: method

No positive control was ever run (read a register whose value is known from Linux on the same unit). Absence of effect was always explained by the most exotic cause available. "DEFINITIVE", "CONFIRMED BLOCKED", "IMPOSSIBLE", "FINAL CONCLUSION" were each followed by more work and new final conclusions. `AGENTS.md` holds contradictory "REAL" values side by side (MP0 base `0x58000` and `0x103D0`) and is fed back to AI agents as premises. Invented terms ("SOS-lock", "NBIO firewall", "freeze zone") were never checked against AMD headers that were at hand. One unit with an unusual BIOS was generalized. Other projects were criticized for an addressing error related to the author's own.

## What the predecessor did well

Working SMU mailbox with safety limits, working PSP GPCOM ring after the base fix, working display through KMDOD, init-step kill switches and a reboot-surviving step marker, and an enormous documented exploration without which this analysis would not exist.

## What this does not show

- That 3D "just works" once offsets are fixed. Correct offsets are necessary, not sufficient: GART/VM, RLC, rings, interrupts, then a WDDM miniport and a user-mode driver remain.
- That no register on this chip needs a special access path (RLC safe mode, PSP). For the registers discussed here the 40 CU patch uses plain `WREG32_SOC15`.
- Offsets were computed from GC 10.1.0 headers, which the kernel also uses for GC 10.1.3. Each must still be confirmed by reading a known value.

## How to verify

Experiment E01 (`tools/diagusb`): boot the unit under Linux, read the registers raw through BAR5 before any driver is loaded (the state Windows inherits), then load `amdgpu` and read them again through the kernel. Expected: `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000`, `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x7` per bank, `SCRATCH_REG0` writable, `GB_ADDR_CONFIG = 0x00100044` after init. Experiment E02 repeats the reads under Windows.

## One-paragraph version for an upstream issue

> `AMDBC250_GC_BASE 0x1260` is a DWORD index, not a byte offset. amdgpu computes `byte = (seg_base + mmREG) * 4` (`SOC15_REG_OFFSET` + `readl(rmmio + reg*4)`). You already found this for MP0 (`0x16000 * 4 = 0x58000`) but never applied it to GC. Correct byte offsets: `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x935C`, `CC_GC_SHADER_ARRAY_CONFIG = 0x89BC`, `GRBM_GFX_INDEX = 0x30800` (seg 1), `GRBM_STATUS = 0x8010`, `CP_ME_CNTL = 0x86D8`, `CP_RB0_BASE = 0xC100`, `SCRATCH_REG0 = 0x30100`. Your "SOS-lock" readbacks are accesses to unrelated or nonexistent registers. Also: stock `SPI_PG = 0x7` is enough for 3D on 24 CUs; the WGP unlock is not a prerequisite, GART/VM and ring bring-up are.
