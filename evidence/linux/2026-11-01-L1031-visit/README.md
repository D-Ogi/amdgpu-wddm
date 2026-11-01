# L1031 visit: memory layout, DCN formats, DP link, PCIe links and the ROM window

Date: 2026-10-31 and 2026-11-01. Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`),
GRUB entry `bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand once per boot). One monitor
on DP-1 at 1920x1200, 59.95 Hz. Wishlist rows: L7 (memory layout), L8 (DP link), L10 (SMU surface),
L35 (ROM read), L44 (format snapshots), L47 DCN201 (plane formats), L49 (FCH link), and the extra
item X2 (DP link rate).

| Run | UTC | amdgpu | Steps |
|---|---|---|---|
| first batch, own boot of about 23:03:30 | 23:04:33 to 23:05:19 | loaded by the batch at about 23:04:45 | L49 (`lspci`), L7, L8, L47, L10, L44 (`out/batch-1031/`) |
| Boot A, boot about 23:48:45 | 23:55:23 | not loaded | L7 before amdgpu, L49 (`lspci`) |
| Boot A | 23:56:16 to about 23:59:15 | loaded at 23:56:16 | L7, L10, L8 link, L47, L35 at 23:57:38, DCN pipes at 23:58:17, flip test with L44 at 23:59:00 |
| Boot B, boot about 00:05:02 | 00:09:14 | not loaded | L49 through the host bridge |
| Boot B | 00:25:29 | loaded at about 00:13:17 | L49 through amdgpu, L7 after amdgpu, X2 |

## The clock

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. The `utc=` stamps and
`steps.txt` carry this skewed clock: subtract 1 h and about 28 s. The UTC times in this file come from
the development PC or from that correction. The boot times come from the uptime in the same files.

## L7: memory layout

- `batch-1031/l7-fb-regs.txt` (through debugfs `amdgpu_regs`) and `boot-a/l7-pre.txt` (through BAR5
  before amdgpu): `GCMC_VM_FB_LOCATION_BASE` `0xF400`, `GCMC_VM_FB_LOCATION_TOP` `0xF5FF` and
  `GCMC_VM_FB_OFFSET` `0x270`. The `MMMC_VM_*` copies read the same values.
- `GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR` reads `0x3D8000` before amdgpu and `0x3D7FFF` after it.
  `GCMC_VM_AGP_BOT` reads 0 before amdgpu and `0xFFFFFF` after it.
- Before amdgpu, the context 0 page table registers of GCVM and MMVM read 0, and the firmware plane of
  HUBP0 is at `0x2_7000_0000` with a pitch of `0x77F`. `/proc/iomem` reserves `0x270000000-0x46fffffff`.
- After amdgpu (`batch-1031/l7-dmesg.txt`): VRAM 8192 MiB at `0xF4_0000_0000`, GART 512 MiB at 0,
  VM size 262144 GB with 4 levels. `l7-vram_mm.txt` reads 8178 MiB free of 8192 MiB. The GTT total
  is 4020457472 bytes.
- `boot-a/l7-vm-info.txt` is empty: the read of `amdgpu_vm_info` returned no data.

## L44 and L47: plane formats and DCN snapshots

`batch-1031/l47-planes.txt`: planes 41 and 45 (primary) and plane 49 (overlay) list the same 18
formats, from XR24 to AB4H. Plane 55 (cursor) lists AR24 alone. Boot A has the same file.

`batch-1031/l44-regs-*.txt` hold 998 registers of DCN pipes 0 and 1 under fbcon, after a mode set in
each of XR24, XB30 and XB4H, and after the last mode set. `scripts/decode-l44.py` decodes their fields
into `batch-1031/l44-decoded.md`. Pipe 0 has a 960x1200 viewport. Pipe 1 is blank
(`HUBP1_DCHUBP_CNTL` `0x000F101B`, `HUBP_BLANK_EN` 1). The other half of the plane is on pipe 3 (see
`boot-a/dcn-pipes.txt` and `../2026-11-01-L1031-display/`), which this batch did not read.

| Field (pipe 0) | fbcon | XR24 | XB30 | XB4H |
|---|---|---|---|---|
| HUBP `SURFACE_PIXEL_FORMAT` | 0x8 | 0x8 | 0xA | 0x18 |
| `CNVC_SURFACE_PIXEL_FORMAT` | 0x8 | 0x8 | 0xA | 0x19 |
| `CROSSBAR_SRC_CB_B` / `CROSSBAR_SRC_CR_R` | 2 / 3 | 2 / 3 | 3 / 2 | 3 / 2 |
| `REFCYC_PER_REQ_DELIVERY` | 0x9BD8 | 0x9BD8 | 0x9BD8 | 0x506F |
| `VREADY_OFFSET` | 0x18B | 0x18B | 0x18B | 0x16E |

`MPCC0_MPCC_CONTROL` (`0xFFFF0423`), `CNVC_CFG0_FORMAT_CONTROL` and `MPCC_OGAM0_MPCC_OGAM_MODE` (0) do
not change with the format. Boot A repeats the three mode sets with a smaller table (`boot-a/l44-*.txt`,
`flip-*.txt`, `dtn-*.txt`).

## L8 and X2: DP link

`boot-a/l8-dp-dbg.txt` (after amdgpu, before the flip test): DP-1 `Current: 4 0x6`, `Verified: 4 0xa`,
`Reported: 4 0xa`. So the link runs 4 lanes at the RBR rate (`0x6`), and 4 lanes at HBR (`0xa`) passed
verification. `max_bpc` 16, `output_bpc` maximum 8.

- The flip test tried to read the same file before and after its mode sets (`x2-link-before.txt`,
  `x2-link-after.txt`). Both reads failed with a wrong path. So no read of the link rate after the mode
  sets exists.
- `boot-b/x2-dplink.txt` reads the DP0 link and DPHY registers, the connector properties and the link
  lines of the kernel log. It holds no link rate.
- Arithmetic, not a measurement: RBR on 4 lanes carries 4 x 1.62 Gbit/s x 0.8 = 5.18 Gbit/s of data.
  1920x1200 at 154 MHz and 24 bit per pixel needs 3.70 Gbit/s. The reason for the choice of RBR is not
  in these files.

## L10: SMU surface

`boot-a/l10-pm-info.txt`: SCLK 1500 MHz, MCLK 450 MHz, VDDGFX 906 mV, VDDNB 806 mV, 56.03 W average
SoC power with the CPU, VCN powered down. It prints `SMC Feature Mask: 0xffffffffffffffff`, which is
not a validated feature read. `l10-pp-all.txt` holds the `pp_dpm_*` levels (SCLK 1000, 1500 and 2000
MHz) and the OD table (SCLK range 1000 to 2000 MHz, VDDC 700 to 1129 mV). `batch-1031/` has the same
reads of the first batch and the raw `gpu_metrics` bytes.

## L49: PCIe links

- `lspci` (`batch-1031/l49-lnk.txt`, `boot-a/l49-pci.txt`): the FCH port `00:15.0` trains at 5 GT/s x2
  (capability 5 GT/s x2). The FCH port `00:15.1` trains at 2.5 GT/s x1 (capability 5 GT/s x1). The GPU
  link reads 16 GT/s x16.
- The APU to FCH link has no visible bridge. `scripts/l49.sh` reads its PCIe core through SMN with the
  table `scripts/l49-smn.regs`. The script names and SMN addresses come from `nbio_7_2_0_offset.h` of
  Linux v6.18, named in the table header. `tools/regcalc` does not hold that header.
- `boot-b/l49-host-raw.txt`: through the index and data pair at offsets 0x60 and 0x64 of the host bridge
  `00:00.0`, with `k10temp` unloaded for the step and loaded again after it. The root port block reads
  vendor 1022, device 13df. The link capability reads 16 GT/s x4, the link status reads 5.0 GT/s x4 with
  the data link layer active, and the target speed reads 5.0 GT/s. The link controller reads a current
  rate of 5.0 GT/s and a partner that supports gen2 only. Its state reads `0x10` in 10 of 10 reads. Port 1
  reads no link.
- `boot-b/l49-gpu.txt`: the same table through debugfs `amdgpu_regs_pcie`. 36 of 37 rows are equal to
  the host-bridge read. One port 0 link controller row reads `0x8A280090` before amdgpu and `0x8A2800A0`
  after it.
- `boot-a/l49-gpu.txt`: the SMN step failed because amdgpu was not loaded. `boot-a/l49-smn-through-debugfs.txt`
  reads the same table through debugfs `amdgpu_regs`. Every entry reads `0xFFFFFFFF`: that file does not
  reach SMN addresses.

## L35: ROM window

`boot-a/l35-probe.json` and `boot-a/l35-read.json` (`scripts/l35.sh`, `scripts/spiread.py`): two reads
of the 16 MiB window at `0xFF000000` through `/dev/mem`, 4.6 s each. Both have the SHA-256
`a6a5e6fb...e93eb`, with no differing 4 KiB page. The window has the `55AA55AA` signature at `0x820000`,
7 firmware volume signatures and the first reset vector byte `0x90`. The LPC bridge configuration did
not change during the reads. The image is not in the repository. This is a read of the mapped ROM
window. It does not prove a direct read of the SPI chip, its identity or its protection state.

## Other observations

- After amdgpu, `scripts/bar5read.py` refuses BAR5 reads while a driver is bound
  (`boot-a/l8-post-amdgpu-log.txt`).
- The kernel logs of these boots have the two `amdgpu_dm_hpd_init` warnings of M811 at each amdgpu load.

## Files

- `out/batch-1031/`: the first batch (`scripts/l1031-batch.sh`), with `steps.txt` and its environment
  (`uname.txt`, `apk.txt`, `modprobe.txt`, `debugfs.txt`).
- `out/boot-a/`, `out/boot-b/`: the reads of Boot A and Boot B. `dmesg-boot-a.txt` is the kernel log
  stream of Boot A.
- `scripts/`: the step scripts and register tables. `scripts/boot-a/` holds the Boot A commands, taken
  from the session tool log. `to-linux.ps1`, `to-windows.sh` and `win-precheck.ps1` steer the stick.
  `linux-xfer.py` and `stream-linux.py` move files and stream output.

## Limits

- One monitor, one mode and one kernel. No pixel output was compared.
- The L44 batch did not read pipe 3. The DP link rate was read only before the mode sets.
- L35 read the mapped window only.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. The monitor's EDID bytes and name are removed
from the connector lists and DTN logs. `sha256.txt` holds the SHA-256 of every file in this directory
except itself.
