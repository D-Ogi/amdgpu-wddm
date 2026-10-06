# L1006 visit 3: pre-driver state and the M.2 link over recorded cold and warm starts (L1, L2, L12)

Date: 2026-10-06, 13:14 to 13:25 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`),
GRUB entry `bc250.mode=network` (amdgpu blacklisted at boot). Five boots, each with a recorded history.
The owner gave consent on 2026-10-06 for cold starts through the smart plug.

## Boot history

The Linux clock of unit A runs 2 h ahead of UTC (the RTC holds local time, see visit 1). The UTC times below
are the Linux times minus 2 h. The plug times in `power-log.jsonl` are UTC from the development PC.

| Boot | How it started | Sweep (UTC) | Uptime at the sweep |
|---|---|---|---|
| `warm-after-windows` | Warm restart out of Windows, our KMD running, through the stick's loader steering | 13:15:14 | 29.5 s |
| `cold1` | `poweroff` 13:16:43, plug 10.0 W (standby) 13:16:59, AC off 13:17:02 to 13:17:34 (32 s) | 13:18:24 | 29.4 s |
| `cold2` | `poweroff` 13:18:54, plug 10.7 W 13:19:09, AC off 13:19:10 to 13:19:43 (33 s) | 13:20:41 | 37.5 s |
| `cold3` | `poweroff` 13:21:12, plug 10.6 W 13:21:27, AC off 13:21:29 to 13:22:01 (32 s) | 13:22:57 | 37.2 s |
| `warm-after-amdgpu` | In the `cold3` boot after its sweep, `modprobe amdgpu` (load at 72.9 s, about 13:23:33), then `reboot` | 13:24:41 | 33.1 s |

The board's jumper powers the machine on when AC returns. No firmware boot entry changed. amdgpu was not
loaded in any boot at the time of its sweep (`amdgpu_loaded 0` in every `boot.txt`).

## Kit

- `scripts/cold-cycle.sh` (development PC): one recorded cold start. It sends a clean `poweroff` over SSH, waits
  until the plug reads less than 20 W, turns the plug off for 30 s, and turns it on again. Every step goes to
  `power-log.jsonl`.
- `scripts/cold-run.sh` (stick, one run per boot): writes `boot.txt`, writes `m2-full.txt` (`lspci -vvv` of
  `00:15.0`, the devices below the port, the NVMe device, the port's `aer_*` and link files in sysfs, and the
  PCIe and NVMe lines of `dmesg`), then runs the E21 collector steps `pre` and `sweep`.
- The E21 collector on the stick is byte identical to `experiments/E21-linux-reference-4/linux_session_collect.sh`
  (SHA-256 `5985507b79c6dd5bcf17c5af930907dd10b75360dd6549648d25763e9d5e3bf5`). Its `sweep.json` (SHA-256
  `4579f78f8de074c56d89f5b0287be289d9716603dabd769655d29ddae77985e4`) and the E03 conservative skip list
  `skip-conservative.txt` (SHA-256 `0fb36721db5fefdf141df95859d5816effdfe1742e58a0e25351db4a8ecc419e`) give
  6313 items per sweep, and the sweep skips 1160 of them. The stick copies of these three files are not in this directory.
- `scripts/stage-c.ps1` and `scripts/pull-c.ps1` (Windows side) copy the kit onto the identified stick and
  pack the outputs.

## Results

Every count comes from `analysis/l1_compare.py`. Its output is `analysis/l1-compare.txt`. A register is a sweep
line with an upper-case name and an 8-digit hex value, the same parse as `scripts/cmp_sweeps.py`
(output `analysis/cmp-pc.txt`). That gives 5021 registers in every sweep. The 132 lower-case alias items of E03
(`rule.*`, `psp_literal.*`) are not in the counts. The script compares values as strings.

### Positive control

`GB_ADDR_CONFIG = 0x00000044` and `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000` in all five sweeps (M5, M4).

### L1: what changes with the power-up

| Pair | Registers that differ |
|---|---|
| `cold1` / `cold2` | 468 |
| `cold2` / `cold3` | 450 |
| `cold1` / `cold3` | 472 |
| `cold1` / `warm-after-windows` | 690 |
| `cold3` / `warm-after-amdgpu` | 506 |
| `warm-after-windows` / `warm-after-amdgpu` | 190 |

- 557 registers vary across the three cold starts: 555 in GC, and `MP0_SMN_C2PMSG_81`, which the sweep reads
  through the MP0 and the MP1 apertures (the same offset `0x58244`).
- They differ by few bits. For each register, the largest bit distance between two cold starts has the median 2,
  and 477 of the 557 differ by at most 3 bits. The widest are timers (`RLC_REFCLOCK_TIMESTAMP_LSB`,
  `RLC_GPU_CLOCK_32`, `SQ_TIME_LO`). Examples: `CB_BLEND*`, `CB_COLOR*`, `PA_CL_VPORT_*`, `SPI_PS_INPUT_CNTL_*`,
  `COMPUTE_*`.
- After a warm restart from a boot in which a driver ran, most of them read 0: 385 of 557 in
  `warm-after-windows`, 359 in `warm-after-amdgpu`. These data do not show whether the restart or the driver
  before it cleared them.
- 166 of the 557 keep their `cold3` value through the amdgpu load and the warm restart into
  `warm-after-amdgpu`. Examples: `COMPUTE_DIM_X = 0xD99B8E42`, `COMPUTE_PGM_LO = 0xCA0E1F23`.

### L2: engine state after a driver and a warm restart

In all five boots, cold and warm alike, the engine registers of L2 read the values M6 gives for the state the
BIOS hands over: `CP_ME_CNTL = 0x15000000`, `CP_MEC_CNTL = 0x50000000`, `RLC_CNTL = 0`, `RLC_STAT = 0`,
`SDMA0/1_F32_CNTL = 1`, `SDMA0/1_STATUS_REG = 0x46DEE557`, `GRBM_STATUS = 0x00003028`, `GRBM_STATUS2 = 0x8`,
`GRBM_STATUS_SE0 = 0x6`, `CP_RB0_BASE = 0xFEDCBAEF`, `CP_RB0_RPTR = CP_RB0_WPTR = 0`,
`MP0_SMN_C2PMSG_64 = 0x80000000`, `C2PMSG_33 = 0x80000000`, `C2PMSG_35 = 0xFFFFFFFF`. Neither our KMD nor
amdgpu leaves an engine running or a ring programmed across a warm restart.

Register contents that no reset touches do survive. `warm-after-windows` holds a compute dispatch:
`COMPUTE_DIM_X/Y/Z = 0x30, 1, 1`, `COMPUTE_NUM_THREAD_X/Y/Z = 0x00400040, 0x00010001, 0x00010001`,
`COMPUTE_PGM_LO = 0x010028C3`, `COMPUTE_PGM_HI = 0`, `COMPUTE_PGM_RSRC1 = 0x202C0001`,
`COMPUTE_PGM_RSRC2 = 0x8C`, `COMPUTE_TMPRING_SIZE = 0x00021500`, `COMPUTE_DISPATCH_INITIATOR = 0x2047`,
`COMPUTE_RESOURCE_LIMITS = 0x01000000`. Under Windows only our driver stack drives the GPU, so this is most
likely the last compute dispatch of the Windows session. In `warm-after-amdgpu` the same registers keep the
`cold3` power-up values. 42 of the 190 registers that differ between the two warm boots are `COMPUTE_*`,
66 are `PA_*` and 55 are `SPI_*`.

### `MP0_SMN_C2PMSG_81`: cold or warm

| Boot | Through MP0 | Through MP1 (later in the same sweep) |
|---|---|---|
| `warm-after-windows` | `0x002B3EF6` | `0x002B3F30` |
| `cold1` | `0x004314EE` | `0x00431529` |
| `cold2` | `0x0043192E` | `0x00431970` |
| `cold3` | `0x00431AC5` | `0x00431B10` |
| `warm-after-amdgpu` | `0x002B2155` | `0x002B2190` |

The value is higher at the second read in every boot, so the register most likely counts. It reads
`0x0043xxxx` after all three cold starts and `0x002Bxxxx` after both warm restarts. The split can be a
different count since the PSP started, not a flag. M6 read `0x002AFC44` in a boot of unrecorded history.

### L12: the M.2 root port `00:15.0`

| Boot | LnkCap | LnkSta | NVMe |
|---|---|---|---|
| all five | 5 GT/s, x2 | 5 GT/s, x2 | `nvme0`, `03:00.0` below the port, partitions read |

- `LnkCtl2` sets the target speed to 5 GT/s in every boot. The kernel reports that the port limits the SSD to
  5 GT/s x2 (the SSD can do 8 GT/s x4).
- The port has no Advanced Error Reporting capability. Its only extended capability is a vendor-specific one at
  `0x100`, and sysfs has no `aer_*` file for it. There are no AER counters to read.
- M21's lost link did not occur in three recorded cold starts and two recorded warm restarts.

## Other observations

- The amdgpu load in the `cold3` boot (`boots/cold3-amdgpu/dmesg.txt`) prints the two HPD warnings of M811 again.
  It reports `active_cu_number 24` (the BIOS state, M4) and once `Fence fallback timer expired on ring sdma0`.
- `analysis/stick-cmp-invalid.txt` is the output of `scripts/cmp.sh` on the stick. It reports 0 differences for
  every pair, which is wrong. The same pipeline under Git Bash on the development PC gives 464 for
  `cold1` / `cold2`. Its `awk '$2 != $3'` compares as numbers the values that look like numbers: `000000E0`
  and `000000E8` both read as 0. Do not use `cmp.sh`.
- `collect/pre.txt` and `collect.log` print "this is not the bc250.mode=readonly entry". The network entry was
  the intended entry for this visit.

## Redaction and hashes

`REDACTED.txt` lists the removed items: the boot ID UUID in each `boot.txt` and run log, and a MAC address and
the stick's USB serial number in the amdgpu kernel log. The `REDACT-BEFORE-COMMIT.txt` files are the
collector's own notes, copied unchanged. `sha256.txt` holds the SHA-256 of every file in this directory except
this README.
