# Prior-art claims confronted with measurements on unit A

Companion to `prior-art.md` and `predecessor-analysis.md`, which argued from source code. This file
uses only what was measured on 2026-09-21 under Linux 6.18.52 (evidence: `evidence/linux/2026-09-21-E01-*`
and `2026-09-21-E03-init-trace/`; facts: `facts.md` M1-M15). Nothing here is about Windows yet.

Method note: offsets produced by other projects' addressing rules were resolved **offline** against
the named registers of our sweep. We did not read addresses that are not named registers, because
a blind read can hang this SoC (see "A hazard nobody reported" below). Eleven literal offsets from
the Keshas driver were read once, in E01, before we knew that.

## The three addressing rules side by side

| Register | True `(base + mm) * 4` | Before driver | After amdgpu | Keshas `base + mm*4` | ZEROAESQUERDA `mm*4` |
|---|---|---|---|---|---|
| `GRBM_STATUS` | `0x08010` | `00003028` | `00003028` | `0x048F0` | `0x03690` |
| `CC_GC_SHADER_ARRAY_CONFIG` | `0x089BC` | `FFF80000` | `FFF80000` | `0x0529C` | `0x0403C` |
| `SPI_PG_ENABLE_STATIC_WGP_MASK` | `0x0935C` | `0000FFFF` | `00000007` | `0x05C3C` | `0x049DC` |
| `CP_ME_CNTL` | `0x086D8` | `15000000` | `00000000` | `0x04FB8` | `0x03D58` |
| `CP_RB0_BASE` | `0x0C100` | `FEDCBAEF` | `000064D0` | `0x089E0` | `0x07780` |
| `SCRATCH_REG0` | `0x30100` | `00000000` | `DEADBEEF` | `0x12100` | `0x08100` |
| `GRBM_GFX_INDEX` | `0x30800` | `00000000` | `00000000` | `0x12800` | `0x08800` |
| `RLC_PG_ALWAYS_ON_WGP_MASK` | `0x3B14C` | `00000003` | `00000003` | `0x1D14C` | `0x1314C` |

Of the 64 offsets the two wrong rules produce for our 32 key GC registers, 53 are not registers of
any block we have headers for. The 11 that are land on unrelated registers:

| Meant to be | Rule | Offset | Really is | Value (before = after) |
|---|---|---|---|---|
| `SPI_PG_ENABLE_STATIC_WGP_MASK` | zero | `0x049DC` | `SDMA0_PG_CTX_LO` | `00000000` |
| `GB_ADDR_CONFIG` | keshas | `0x061D8` | `SDMA1_PG_CNTL` | `00000000` |
| `CP_RB0_BASE` | keshas | `0x089E0` | `VGT_TF_MEMORY_BASE_HI` | `00000000` |
| `CP_HQD_ACTIVE` | keshas | `0x0910C` | `SPI_DSM_CNTL` | `00000000` |
| `CP_CPF_STATUS` | keshas | `0x04AFC` | `SDMA0_AQL_STATUS` | `00000003` |
| `CP_CPF_STATUS` | zero | `0x0389C` | NBIO `BIF_BX_DEV0_EPF0_VF0_GPU_HDP_FLUSH_DONE` | `00000000` |
| `CP_MEC_CNTL` | zero | `0x038B4` | NBIO `BIF_BACO_EXIT_TIMER1` | `04000200` |
| `CP_RB0_RPTR` | zero | `0x03D80` | `HDP_NONSURFACE_BASE` | `F4000000` |
| `GRBM_STATUS_SE0` | zero | `0x03694` | NBIO `RCC_DEV0_EPF0_RCC_ERR_LOG` | `00000000` |
| `SCRATCH_REG0` | zero | `0x08100` | `GRBM_SCRATCH_REG0` | `FA114F7C` |
| `SCRATCH_REG1` | zero | `0x08104` | `GRBM_SCRATCH_REG1` | `DCE6F8B8` |

The last two rows are the instructive ones: with the `mm*4` rule the "scratch register" happens to
be a *different, real* scratch register. A write/read-back test passes there while every other
address is wrong. One positive control is not enough; the control has to be a register whose
value is known from an independent source (we used `0xDEADBEEF` left by the kernel's ring test,
the FB location against dmesg, and the published stock CU values: `facts.md` M1).

## Claim by claim

### Keshas-dev/AMD-BC-250-Windows-Driver

| Claim | What the hardware says | Status |
|---|---|---|
| The CU registers sit at `0x5C3C` / `0x9C1C` (earlier `0x34FC` / `0x3264`) | They sit at `0x935C` / `0x89BC` and hold the published stock values. `0x5C3C`, `0x9C1C`, `0x3264` read `0`; `0x34FC` reads `0x2000` (E01 claims table) | REFUTED by measurement (M1, M9) |
| Writes do not stick, the registers are locked by firmware ("SOS-locked") | With no driver loaded, a user-space mmap of BAR5 wrote and read back `SCRATCH_REG0` and switched `GRBM_GFX_INDEX` banks. The writes that "did not stick" went to addresses that are not registers, or to unrelated ones (table above). We have not written the CU registers ourselves, so whether *those* accept a host write before RLC/PSP setup is still open | Locking: unsupported. CU write behaviour: untested |
| Linux only manages because of debugfs privilege or an early boot window | Our reads and writes used neither debugfs nor the driver, minutes after boot (M2, M3). debugfs and raw MMIO return identical values for all 58 probed registers | REFUTED by measurement |
| "NBIO is locked at EFI boot"; `0xC100` is an NBIO ID register | `0xC100` is `CP_RB0_BASE`. Before the driver it reads `0xFEDCBAEF`, which looks like an ID or magic number and probably was taken for one; after init it holds the ring address `0x64D0`. Real NBIO registers read fine (`RCC_CONFIG_MEMSIZE = 0x2000`, 155 BIF/RCC registers in E03) | REFUTED by measurement |
| KIQ / ring base registers "are hardwired to 0" (`0xDA60`, `0xE060`) | Those offsets are not registers and read `0`. The real KIQ runs: rptr = wptr = `0xE00` after init, as do gfx, 8 compute and 2 SDMA rings (M12) | REFUTED by measurement |
| The GPU cannot do useful work without the 40 CU / WGP unlock | Unit A initializes and runs all 12 rings in the stock state, `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000` throughout (M4, M7) | REFUTED by measurement |
| `SPI_PG_ENABLE_STATIC_WGP_MASK = 0x7` is the BIOS state | The BIOS leaves `0x0000FFFF`; the value becomes `0x7` during amdgpu's init (M4). The init trace contains no host write to this register (DWORD index `0x24D7`) and none to `CC_GC_SHADER_ARRAY_CONFIG`, so the change comes from firmware or a command stream, not from a plain MMIO write | Corrected |

### ZEROAESQUERDA/BC250-windowsDriverTest

| Claim | What the hardware says | Status |
|---|---|---|
| Register byte offset = `mm * 4` (e.g. `CP_RB0_BASE` at `0x7780`) | `0x7780` is not a named register. `CP_RB0_BASE` is at `0xC100` (before `FEDCBAEF`, after `000064D0`) | REFUTED by measurement |
| BAR5 is essentially the SMN mailbox; real registers need another path | 5797 named registers of GC, MMHUB, OSSSYS, HDP, NBIO and MP0/MP1 were read through BAR5 before any driver, 5021 after | REFUTED by measurement |
| Every unverified MMIO path must be gated | Borne out more strongly than the author knew: a single read of the wrong address hung our unit (below) | CONFIRMED as a practice |

### Keshas-dev/AMD-BC-250-PSP-Driver

| Claim | What the hardware says | Status |
|---|---|---|
| MP1/SMU mailbox at `MP1_BASE 0x16000` + `0xA68` in BAR5 | `0x16000` is a DWORD index. `MP1_SMN_C2PMSG_90` is at `0x58A68` and reads `1` (SMU answered) before any driver; `0x16A68` is not a named register | REFUTED by measurement |
| "SOS does not support ring commands" (first version) | Withdrawn by the author after moving to `0x58000`. Consistent with our trace: amdgpu's init uses the PSP ring on this unit (MP0 `C2PMSG_64`: `80000000` before, `80020000` after) | Already self-corrected |
| PSP mailbox literals `0x1056C`-`0x10614` | These belong to the PSP PCI function's own BAR (`0xFD600000` in that code), not to GPU BAR5. Different device, not comparable, not tested by us | Not assessed |

## A hazard nobody reported

Reading `MMHUB.MMEA0_ADDRDEC0_BASE_ADDR_CS0` (BAR5 `0x68594`, a memory address decoder register that
amdgpu never touches) hangs unit A instantly: black screen, no network, power cycle needed. It
happened three times before we had the register's name; the log that names it is
`2026-09-21-E03-init-trace/sweep-before-run1-GC-complete-then-hang.log`.

Consequences:

- A "scan the BAR and see what answers" approach, which several prior-art tools use, will sooner or
  later lock the machine up, and the lock-up looks exactly like "the firmware protects this range".
  Some reported "SOS lock" symptoms may be this.
- Our rule from now on: read only named registers, from an allow-list of families the Linux driver
  itself uses. Everything else is explored one family at a time, streamed over SSH, last.

## What this does not show

- Nothing about Windows. Whether the same MMIO reads and writes work from a Windows kernel driver
  on this unit is experiment E02.
- Nothing about writing the CU configuration registers: we did not write them.
- One unit, one BIOS (P3.00), one kernel.
