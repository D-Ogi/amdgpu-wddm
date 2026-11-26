# GRBM window and unnamed GC dwords, read through MMIO (2026-11-26)

Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`), GRUB entry 4 `bc250.mode=network`
(amdgpu blacklisted at boot, loaded by hand). Two starts. All accesses are reads. Nothing was written to the GPU.
The only other changes were the grubenv of the stick (`scripts/steer.ps1`, `scripts/restore-stick.sh`, restored
byte for byte, SHA-256 `e9aab6c0...` before and after) and `enable` of the PCI function in sysfs before the raw reads.

The Linux clock of unit A reads local time (UTC+1). The `utc=` stamps in `out/` are one hour ahead of UTC.

## Why

A community report on the BC-250 Discord claims that BAR5 dwords `0x203A` and `0x203B` select the GFX pipe mode
(`0x2` RDNA at boot, `0x0` and `0x1` PS4 GCN compatibility modes), read and written through the KIQ
(`amdgpu_kiq_rreg`, `amdgpu_kiq_wreg`). That report is from another project and is not a fact here (CLAUDE.md
rule 8). These reads measure what MMIO gives at these offsets. The owner also asked for the whole GRBM window
and for the GC offsets that the E03 sweep did not read.

## Tables

`tools/regcalc` names no GC register at `0x203A` or `0x203B`. The tables are generated with `regcalc.py reverse`
(`scripts/gc-window-names.txt`: the 1256 named GC registers in dwords `0x2000`-`0x2FFF`).

| Table | Content |
|---|---|
| `scripts/controls.regs` | Positive controls: `GRBM_CHIP_REVISION`, `GRBM_SCRATCH_REG0`, `SPI_PG_ENABLE_STATIC_WGP_MASK`, `GB_ADDR_CONFIG`, `CC_GC_SHADER_ARRAY_CONFIG` |
| `scripts/raw.regs` | The controls, `0x203A`, `0x203B` and `GRBM_STATUS` |
| `scripts/grbm.regs` | Every dword of `0x2000`-`0x2083` (GRBM: named and unnamed, 132 reads). No name in it is an indexed data port |
| `scripts/gc-holes.regs` | Every unnamed dword of `0x2084`-`0x2FFF` (2751 reads) |

`regstream.py` (now `tools/linux-session/regstream.py`) writes each name and offset before the read and the value
after it, so a hang leaves the address as the last line. Mode `raw` reads the sysfs `resource5` mmap, mode
`debugfs` reads amdgpu's `amdgpu_regs`. No read hung.

## Starts

| Start | IOMMU in Setup | amdgpu | Files |
|---|---|---|---|
| A (warm restart from Windows, about 03:03Z) | on | probe failed `-110`: the PSP refused every `LOAD_IP_FW` with `0xFFFF0008`, the known IOMMU effect | `out/boot-a/` |
| B (cold start at about 03:07Z, after the owner switched IOMMU off) | off | bound | `out/boot-b/` |

The owner switched IOMMU on again after start B. Windows came back on its route (`scripts/win-check.ps1`).

## Results

1. Positive controls. `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000`, `GB_ADDR_CONFIG = 0x00000044` and
   `GRBM_STATUS = 0x00003028` in every read. `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x0000FFFF` before amdgpu and
   `0x00000007` after it. These are the values of M4.
2. `0x203A` and `0x203B` read `0xFFFFFFFF` in each of the five reads: before amdgpu (A and B), after the failed
   probe (A), and through debugfs after a full amdgpu init (B).
3. In the same GRBM window, the unnamed dwords `0x202C`-`0x2039` and `0x203C`-`0x203E` read `0x00000000`. The
   unnamed dwords `0x2048`-`0x204F` (after `GRBM_SCRATCH_REG7`) also read `0xFFFFFFFF`.
4. Unnamed GC dwords `0x2084`-`0x2FFF`: 2751 reads in each start, no hang. Start A (after the failed probe):
   57 nonzero. Start B (amdgpu bound): 60 nonzero. Some change between the two states (for example `0x2E2F`-`0x2E33`,
   `0x262D`, `0x26A9`). Some hold fixed patterns (`0xBAADADD9` at `0x27CC`-`0x27CF` and `0x27DC`-`0x27DF`,
   `0xA9876543` at `0x2A95`-`0x2A96`).

## What this shows and what it does not

- MMIO cannot read `0x203A` and `0x203B` on this unit in these states. The two offsets behave differently from
  their unnamed neighbours. This agrees with a register that only a CP or KIQ access can reach, but it does not
  prove that such a register exists, and it does not show its value or its function.
- The value `0x2` and the mode claims are not measured. Wishlist row L58 holds the CP and KIQ read for a later
  Linux session.
- The meaning of the nonzero unnamed GC dwords is not known. They are values only.
