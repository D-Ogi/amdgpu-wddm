# Facts

The only list of established facts in this project. Rules and statuses: `01-evidence-rules.md`. Newest first within each section. Nothing here is `MEASURED` or `CONFIRMED` until hardware evidence exists in `evidence/`.

## Hardware behaviour (our unit)

_No measurements yet. The first entries will come from experiment E01._

## Derived from source code (not measured by us)

| # | Claim | Status | Source | Date |
|---|---|---|---|---|
| S4 | The ASUS USB-AC58 Wi-Fi dongle (`0b05:19aa`) is supported by the in-kernel `rtw88_8822bu` driver; module and firmware are present in the Alpine 3.24.2 modloop used by the diagnostic USB | HYPOTHESIS | kernel `rtw8822bu.c` id table; modloop listing | 2026-09-21 |
| S3 | Stock values: `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000`, `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x7` (24 CU); after amdgpu init `GB_ADDR_CONFIG = 0x00100044` | HYPOTHESIS | `bc250-40cu-unlock` README; `CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN` in `gfx_v10_0.c` | 2026-09-21 |
| S2 | All GPU registers (GC, MMHUB, HDP, OSSSYS, MP0/MP1, NBIO) are reached through BAR5, 512 KiB | HYPOTHESIS | `amdgpu_device.c` maps `pci_resource(5)`; all regcalc probe offsets fall below `0x80000` | 2026-09-21 |
| S1 | BAR5 byte offset of a register = `(IP segment base + mm offset) * 4`; GC segment bases are `0x1260` and `0xA000` | HYPOTHESIS (certain as a reading of the source; hardware confirmation pending E01) | `soc15_common.h`, `amdgpu_reg_access.c`, `cyan_skillfish_ip_offset.h` | 2026-09-21 |

## Refuted claims from prior art

| # | Claim | Status | Why | Date |
|---|---|---|---|---|
| R3 | "KIQ_BASE / KIQ_CNTL / CP_RING0_BASE_LO registers exist and are hardwired to 0" | REFUTED (by source) | No such registers in `gc_10_1_0_offset.h`; pinned by `test_regcalc.py` | 2026-09-21 |
| R2 | "Linux can write these registers only thanks to debugfs privilege / an early boot window" | REFUTED (by source) | debugfs uses the driver's normal accessors; the unlock patch runs at ordinary module init; a runtime tool exists | 2026-09-21 |
| R1 | "`SPI_PG_ENABLE_STATIC_WGP_MASK` lives at BAR5+0x5C3C (or 0x34FC), `CC_GC_SHADER_ARRAY_CONFIG` at 0x9C1C (or 0x3264)" | REFUTED (by source) | regcalc: `0x935C` and `0x89BC`; details in `predecessor-analysis.md` | 2026-09-21 |

Claims such as "registers are SOS-locked" or "NBIO is locked at EFI boot" are neither confirmed nor refuted as hardware behaviour: they were measured at wrong offsets and are simply unsupported. E01 will produce the first real data.
