# Facts

The only list of established facts in this project. Rules and statuses: `01-evidence-rules.md`. Newest first within each section. Nothing is `MEASURED` or `CONFIRMED` without hardware evidence in `evidence/`.

## Hardware behaviour (our unit)

Unit A: ASRock BC-250, BIOS P3.00 (12/09/2021), PCI `1002:13FE`. All entries below: evidence `evidence/linux/2026-09-21-E01-diagusb-run-001/` (diagnostic USB, probes id `79152824`, kernel 6.18.52-0-lts), 2026-09-21.

| # | Claim | Status | Detail |
|---|---|---|---|
| M1 | The regcalc offsets address the registers they are named after (S1, S2 hold on hardware) | CONFIRMED | Independent controls, all read through the raw sysfs `resource5` mmap: (a) after amdgpu's ring test `SCRATCH_REG0` at `0x30100` holds `0xDEADBEEF`, the value the kernel's GFX ring test writes to the register it calls `SCRATCH_REG0`; (b) `MMMC_VM_FB_LOCATION_BASE/TOP = 0xF400/0xF5FF` and `HDP_NONSURFACE_BASE = 0xF4000000` agree with dmesg `VRAM: 8192M 0x000000F400000000 - 0x000000F5FFFFFFFF`; (c) `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000` and, after init, `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x7` are the stock values published independently by `bc250-40cu-unlock`; (d) driver-owned registers change the way the driver's init implies: `CP_ME_CNTL 0x15000000 -> 0`, `CP_MEC_CNTL 0x50000000 -> 0`, `RLC_CNTL 0 -> 1`, page table base and IH ring base `0 ->` non-zero, `CP_RB0_RPTR == CP_RB0_WPTR = 0x600`, `IH_RB_RPTR == IH_RB_WPTR = 0xAA0`. Raw reads and debugfs `amdgpu_regs` reads agree for all 58 registers, which shows the two access paths are equivalent but is not by itself a naming control (both take the same byte offset) |
| M2 | The GPU's MMIO is readable and writable from the host before any GPU driver has loaded | MEASURED | No driver bound (`driver: ''`, PCI command `0006`). `GRBM_STATUS = 0x00003028`; `SCRATCH_REG0` read back both written patterns (`0xCAFEDEAD`, `0x5A5AA5A5`) and was restored; 28 of 58 registers non-zero. Same method as M1, so the pre-driver values are trustworthy. Linux only: says nothing yet about Windows (E02) |
| M3 | `GRBM_GFX_INDEX` bank select works from raw MMIO before the driver | MEASURED | SE0/SA0 and SE0/SA1 selected and restored to broadcast; both banks read the same values (M4) |
| M4 | This unit runs the stock compute-unit configuration | MEASURED | `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000` in both shader arrays, before and after amdgpu. `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x0000FFFF` as the BIOS leaves it, `0x00000007` after amdgpu init. `RLC_PG_ALWAYS_ON_WGP_MASK = 0x3`, `GC_USER_SHADER_ARRAY_CONFIG = 0`, `CC_RB_BACKEND_DISABLE = 0` throughout. No sign of a 40 CU unlock in the state the BIOS hands over |
| M5 | `GB_ADDR_CONFIG` reads `0x00000044`, before and after amdgpu init, raw and through debugfs | MEASURED | Differs from `CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN = 0x00100044` in bit 20. Why is open; do not use the golden value as a positive control for this register |
| M6 | State handed over by the BIOS: CP halted, RLC stopped, no page table, SMU answering | MEASURED | Before the driver: `CP_ME_CNTL = 0x15000000`, `CP_MEC_CNTL = 0x50000000`, `RLC_CNTL = 0`, `MMVM_CONTEXT0_PAGE_TABLE_BASE = 0`, `IH_RB_BASE = 0`, `CP_RB0_BASE = 0xFEDCBAEF`; `MP1_SMN_C2PMSG_90 = 1`; `MP0_SMN_C2PMSG_81 = 0x002AFC44` and `C2PMSG_64 = 0x80000000`. VRAM carve-out already programmed: `MMMC_VM_FB_LOCATION_BASE/TOP = 0xF400/0xF5FF`, `HDP_NONSURFACE_BASE = 0xF4000000` |
| M7 | Mainline amdgpu initializes this unit without patches | MEASURED | `modprobe amdgpu` returned 0 after 8.5 s; 12 rings; VRAM 8192M at `0xF400000000`, GTT 3834M; firmware ME 0x63, PFP 0x94, CE 0x25, RLC 0x0D, MEC 0x90, SDMA 0x34. After init `CP_ME_CNTL = 0`, `CP_MEC_CNTL = 0`, `RLC_CNTL = 1`. The only `*ERROR*` lines are display HPD / dummy IRQ noise |
| M8 | The IP discovery table of the unit matches `cyan_skillfish_ip_offset.h` for the blocks we use | MEASURED | GC 10.1.3 bases `0x1260, 0xA000, 0x02402C00`; MMHUB 2.0.3 `0x1A000`; OSSSYS 5.0.1 `0x10A0`; HDP 5.0.1 `0xF20`; MP0/MP1 11.0.8 `0x16000`; NBIF 2.1.1. Full table in `report.json` |
| M9 | What the predecessor's offsets really hold on hardware | MEASURED | `0x3260` ("GRBM_STATUS") = 0 while the real `GRBM_STATUS` at `0x8010` = `0x3028`; `0x9C1C`, `0x5C3C`, `0xDA60`, `0xE060` = 0; `0x3D64` = `0xFFFFFFFF`; `0x34D0` = `0xBA062100`; `0x4A74` = `0xFFFBD9FB`. None of them behaves like the register it was taken for |
| M10 | The ASUS USB-AC58 works on the BC-250 with the stock Alpine kernel: WPA2-PSK association, DHCP, SSH | MEASURED | `wpa_state=COMPLETED`, CCMP, 2437 MHz, first configured network; SSH session from the dev PC used to collect this evidence. Observed live, not part of the evidence directory |
| M11 | Idle in a bare console the GPU sits at 81-82 C and 60-61 W | MEASURED | hwmon `temp1_input`, `power1_average`, three samples about 4 minutes after boot; no fan sensor exposed. Observed live over SSH. Cooling must be sorted out before long sessions |

## Derived from source code (not measured by us)

| # | Claim | Status | Source | Date |
|---|---|---|---|---|
| S4 | (see M10: MEASURED) The ASUS USB-AC58 Wi-Fi dongle (`0b05:19aa`) is supported by the in-kernel `rtw88_8822bu` driver; module and firmware are present in the Alpine 3.24.2 modloop used by the diagnostic USB | HYPOTHESIS | kernel `rtw8822bu.c` id table; modloop listing | 2026-09-21 |
| S3 | (see M4 for the CU registers: MEASURED as stated; M5: the `GB_ADDR_CONFIG` part does not hold on unit A) Stock values: `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000`, `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x7` (24 CU); after amdgpu init `GB_ADDR_CONFIG = 0x00100044` | HYPOTHESIS | `bc250-40cu-unlock` README; `CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN` in `gfx_v10_0.c` | 2026-09-21 |
| S2 | (see M1: CONFIRMED for the 58 probed registers) All GPU registers (GC, MMHUB, HDP, OSSSYS, MP0/MP1, NBIO) are reached through BAR5, 512 KiB | HYPOTHESIS | `amdgpu_device.c` maps `pci_resource(5)`; all regcalc probe offsets fall below `0x80000` | 2026-09-21 |
| S1 | (see M1: CONFIRMED on hardware) BAR5 byte offset of a register = `(IP segment base + mm offset) * 4`; GC segment bases are `0x1260` and `0xA000` | HYPOTHESIS (certain as a reading of the source; hardware confirmation pending E01) | `soc15_common.h`, `amdgpu_reg_access.c`, `cyan_skillfish_ip_offset.h` | 2026-09-21 |

## Refuted claims from prior art

| # | Claim | Status | Why | Date |
|---|---|---|---|---|
| R3 | "KIQ_BASE / KIQ_CNTL / CP_RING0_BASE_LO registers exist and are hardwired to 0" | REFUTED (by source) | No such registers in `gc_10_1_0_offset.h`; pinned by `test_regcalc.py` | 2026-09-21 |
| R2 | "Linux can write these registers only thanks to debugfs privilege / an early boot window" | REFUTED (by source) | debugfs uses the driver's normal accessors; the unlock patch runs at ordinary module init; a runtime tool exists | 2026-09-21 |
| R1 | "`SPI_PG_ENABLE_STATIC_WGP_MASK` lives at BAR5+0x5C3C (or 0x34FC), `CC_GC_SHADER_ARRAY_CONFIG` at 0x9C1C (or 0x3264)" | REFUTED (by source and by measurement, M1 + M9) | regcalc: `0x935C` and `0x89BC`; details in `predecessor-analysis.md` | 2026-09-21 |

Claims such as "registers are SOS-locked" or "NBIO is locked at EFI boot" are neither confirmed nor refuted as hardware behaviour: they were measured at wrong offsets and are simply unsupported. Under Linux nothing is locked before the driver loads (M2, M3); the same reads under Windows are experiment E02.
