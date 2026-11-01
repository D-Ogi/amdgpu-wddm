# L1031 display lane: DCN 2.0.1 reads under amdgpu, underflow and INUSE sampling, overlay and flip states

Date: 2026-10-31 and 2026-11-01. Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`),
GRUB entry `bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand once per boot). One monitor
on DP-1 at 1920x1200, 59.95 Hz. Wishlist rows: L8 (firmware and driver DCN state), L22 (underflow),
L28 (flip and VSTARTUP interrupts), L29 (`SURFACE_INUSE`), L30 (single-pipe reference), L47 DCN201
(overlay), L48 DCN201 (flip timing).

| Boot | UTC | amdgpu | Steps |
|---|---|---|---|
| A | 23:51:37 | not loaded | L8 GOP dump, 615 BAR5 reads (`out/boot-a/l8-gop-bar5.txt`) |
| B | 00:10:12 | not loaded | D0: GOP tables D and E, 579 BAR5 reads (`out/boot-b/d0-gop-de.txt`) |
| B | 00:20:18 to 00:24:50 | loaded | D1 (L8), D2 (L22), D4 (L29), D5 (L47), D6 and D6b (L48), D3 (L28) and its control |
| C | 00:51:40 to 00:53:25 | loaded with `dcdebugmask=0x1` | D7 (L30), then D2, D4 and D6b again |

## The clock

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. The `utc=` stamps of the
lab files carry this skewed clock: subtract 1 h and about 28 s. The UTC times in this file come from
the development PC. The `mono_ns` stamps are nanoseconds since boot.

## Method

`scripts/dcnregs.py` reads named DCN registers through debugfs `amdgpu_regs` and decodes their fields
from `dcn_2_0_1_sh_mask.h`. `scripts/dcnsample.py` polls a set of registers and writes a `C` line each
time a value changes, with the scan position and frame count of OTG0, plus one `S` line per second
and an `E` line at the end. Before amdgpu loads, `scripts/bar5dump.py` reads the tables of
`scripts/tables/` from BAR5 directly. `scripts/gen-tables.py` writes those tables through
`tools/regcalc`. `scripts/stream.py` runs a lab command and streams its output to the development PC.

## L8: firmware state and driver state

- Before amdgpu (Boot A, Boot B): the firmware lights HUBP0 alone, as in M86. The two dumps carry the
  stream header with the check `lsmod | grep -c ^amdgpu` = 0.
- After amdgpu (Boot B, `l8-pipes.txt`, `l8-dcnall.txt`): pipes 0 and 3 are live, each with a
  960x1200 viewport. `MPCC0_MPCC_BOT_SEL` reads 3, and `MPCC0_MPCC_OPP_ID` and `MPCC3_MPCC_OPP_ID`
  read 0. So amdgpu splits the 1920x1200 plane over two pipes that blend into OPP 0 (an MPC split).
  `ODM0_OPTC_DATA_SOURCE_SELECT` reads 0. The DCN 2.0.1 header has only the `OPTC_SEG0_SRC_SEL` field
  in that register.

## L30: single-pipe reference (Boot C)

With `dcdebugmask=0x1`, `l30-pipes.txt` reads one live pipe: HUBP0 at 1920x1200, `split=no`.
`MPCC0_MPCC_BOT_SEL` reads `0xF`, `ODM0_OPTC_DATA_SOURCE_SELECT` reads 0 and `OTG0_OTG_CONTROL` reads
`0x80011311`, the firmware value of M86. HUBP3 reads `0x000F103A` (`HUBP_IN_BLANK`, `HUBP_VTG_SEL` 3),
also the firmware value. `l30-keyregs.txt`, `l30-dcnall.txt` and `l30-dtn.txt` hold the key registers,
the full table and the DTN log of this layout.

## L22: underflow during format changes (D2)

`scripts/d2-l22-underflow.sh` samples `HUBP0_DCHUBP_CNTL`, `HUBP3_DCHUBP_CNTL`, `OTG0_OTG_STATUS` and
`DCHUBBUB_CTRL_STATUS` for 10 s. Meanwhile it runs three `modetest` mode sets: XR24, XB4H (FP16), XR24.

| Boot | Samples | Change lines | `HUBP_UNDERFLOW_STATUS` not zero |
|---|---|---|---|
| B (two pipes) | 809279 | 198863 | 0 |
| C (one pipe) | 795293 | 198438 | 0 |

- The mode sets are in the streams: blank rows of HUBP0 (`0x000F110B`, `HUBP_BLANK_EN` and
  `HUBP_IN_BLANK`) and a restart of the OTG0 frame count.
- The flip markers and the `modetest` output that the script adds to the same file are not in it. So
  the stream does not tell which format each part of the run had. The same `modetest` commands set
  XB4H in the visit (`../2026-11-01-L1031-visit/`, L44).
- These are valid format changes. They are positive controls. They are not the deliberately bad flip
  that L22 asks for.

## L29: SURFACE_INUSE against EARLIEST_INUSE (D4)

`scripts/d4-l29-inuse.sh` samples the six address and INUSE registers of HUBPREQ0 at 10 kHz for 10 s
while `modetest -v` flips the primary plane.

| Boot | Samples | `DCSURF_SURFACE_INUSE` not zero | Address changes | `EARLIEST_INUSE` took the new address |
|---|---|---|---|---|
| B | 100000 | 0 | 586 | 479 |
| C | 100000 | 0 | 489 | 480 |

`DCSURF_SURFACE_INUSE` and `_HIGH` read 0 in every sample, as in M89, now through debugfs and at a
higher rate. `DCSURF_SURFACE_EARLIEST_INUSE` takes the new `PRIMARY_SURFACE_ADDRESS` after the next
frame start in most changes. When the address changes again before that, it skips the address. These
reads do not explain why `SURFACE_INUSE` reads 0.

## L28: flip and VSTARTUP interrupts (D3)

`scripts/d3-l28-irq.py` sets `SURFACE_FLIP_INT_MASK` in `HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT`
(`0x00050000` to `0x00050001`) and `OTG_VERTICAL_INTERRUPT0_INT_ENABLE` in
`OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL` (`0x00001000` to `0x00001100`). It counts the amdgpu MSI-X
interrupts over the same windows as a 5 s `modetest`, then restores both registers. Result
(`l28-irq.txt`): 2 interrupts in the short window right after the enable, 28 in total. After the enable,
`OTG_VERTICAL_INTERRUPT0_INT_STATUS` reads 1.

The control (`scripts/d3c-l28-control.py`, `console-l28-control.txt`) runs the same windows with no
register write: 0 interrupts after the enable step and 29 and 31 in total. So the two enables add no
interrupt rate that this count can see over the run. This run does not tell the source of the 2 early
interrupts. The `RESULT` line of `l28-irq.txt` ("the enabled vectors fire") comes before the control
and does not hold. This is a scoped result: it does not prove that these sources cannot fire with
another interrupt routing.

## L47 DCN201: primary plane and overlay (D5)

`scripts/d5-l47-overlay.py` holds `modetest -s 64@58:1920x1200 -P 49@58:320x200+100+100@AR24` and
reads every pipe (`l47-overlay.txt`):

- Four live pipes: 0 and 2 at 160x200 (the overlay), 1 and 3 at 960x1200 (the primary plane). amdgpu
  splits both planes.
- MPCC tree: MPCC0 bottom 2, MPCC2 bottom 3, MPCC3 bottom 1, MPCC1 bottom `0xF`.
- `SURFACE_PIXEL_FORMAT` 8 on all four HUBPs. `CNVC_CFGn_FORMAT_CONTROL` reads `0x02000101`
  (`ALPHA_EN` 1) on all four.

No pixel output was checked. The alpha blend result, the CSC banks and the YUV formats of L47 were
not tested.

## L48 DCN201: flip control states (D6, D6b)

`scripts/d6b-l48-timing.py` holds the overlay `modetest` of D5 with `-v` (page flips) and samples the
`DCSURF_FLIP_CONTROL` registers, `OTG0_OTG_MASTER_UPDATE_LOCK` and `MPCC0_MPCC_STATUS` at 5 kHz. The
second `modetest` of the script (flips of the primary plane) failed in both boots ("failed to enable
plane: Permission denied", then "select timed out"). The flips in these files come from the overlay
`modetest -v`.

Change lines of `l48b-timing.txt`, by the value of `HUBPREQ0_DCSURF_FLIP_CONTROL` and of
`OTG0_OTG_MASTER_UPDATE_LOCK`:

| Boot | `SURFACE_FLIP_PENDING`, OTG lock 0 | clear, OTG lock 0 | `HUBPREQ_MASTER_UPDATE_LOCK_STATUS`, OTG lock `0x101` | other |
|---|---|---|---|---|
| B | 271 | 145 | 145 | 14 |
| C | 469 | 243 | 11 | 8 |

The other lines are transitions: one of the two registers has changed and the other has not yet. The script read the sampler's pipe
only after the sampler ended. The pipe filled at 64 KiB, so each file holds only the first 2 s (Boot B)
or 4 s (Boot C) of the 15 s run and has no `E` line. `l48-timing.txt` (D6, Boot B) has the same limit
and watches `DCSURF_SURFACE_CONTROL` in place of the flip control.

These files hold the lock, pending and clear states of normal flips. They do not test a flip that
straddles VUPDATE, triple buffering, attribute changes at the same address or the other cases of L48.

## Files

- `out/boot-a/l8-gop-bar5.txt`, `out/boot-b/d0-gop-de.txt`: the streamed copies of the GOP dumps.
- `out/boot-b/`: D1 (`l8-dcnall.txt`, `l8-pipes.txt`), D2 (`l22-underflow.txt.gz`), D3 (`l28-irq.txt`,
  `console-l28-control.txt`), D4 (`l29-inuse.txt`), D5 (`l47-overlay.txt`), D6 (`l48-timing.txt`),
  D6b (`l48b-timing.txt`).
- `out/boot-c/`: D7 (`l30-*`), D2, D4 and D6b again, `d4-console.txt` and `d6b-console.txt` (the
  console output of those steps), `kmsg-bootC.txt` (the kernel log stream of Boot C).
- The two `l22-underflow.txt.gz` files are two runs, each as one gzip file. The Boot B file is the
  gzip copy that the lab wrote. Its content is the same as the uncompressed copy of 17870699 bytes
  that the development PC also pulled, which is not copied here. The Boot C file is the repeat run.
- `scripts/`: the step scripts, the samplers and the register tables.

## Limits

- One monitor, one mode, one kernel. No pixel output was captured or compared.
- The D6 and D6b files are cut at 64 KiB of sampler output.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. The monitor's EDID bytes and the monitor name are
removed from the DTN logs and the connector lists. `sha256.txt` holds the SHA-256 of every file in this
directory except itself.
