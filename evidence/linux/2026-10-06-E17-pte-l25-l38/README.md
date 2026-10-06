# L1006 visit 2: E17 page table entries, L25 two clients, L38 display audio

Date: 2026-10-06, about 12:54 to 13:03 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel
`6.18.52-0-lts`), GRUB entry `bc250.mode=network` (`out/cmdline.txt`), amdgpu loaded by hand once,
42.5 s after boot. This is the second boot of the stick on this day. The first one is
[visit 1](../2026-10-06-L1006-fan-vcn-membw/README.md).

## The clock of this boot

The Linux clock read 2 h ahead of UTC, as in visit 1 (the RTC holds local time). `out/end-date.txt` says
`15:02:34 UTC`, which is 13:02:34 UTC. Every time stamp in `logs/` and in the register snapshots uses
the skewed clock. The UTC times in this file are those times minus 2 h. Trace time stamps are seconds
since boot.

## How the batch ran

The owner allowed writes to the stick on 2026-10-06. The operator staged the batch on the stick before the boot
(`scripts/stage-b2.ps1` copies it to `\l1006b` on the stick, after an identity check of label, file
system, bus and size). On Linux, `scripts/run-b2.sh` runs one phase per call and writes every output
to the stick. After the return to Windows, `scripts/pull-b2.ps1` packed the outputs for the development
PC. `logs/` holds the console output of each phase.

The E17 phases run the kit in `experiments/E17-linux-reference-3` (`session.sh`, `e17_vm.py`,
`pt_walk.py`, `pte_bits.json`, `vmlists.json`) and E13's `dispatch.py`, `info.py` and `regs2.py`.
SHA-256 of the files that ran on the target:

| File | SHA-256 | Same as the repository after this commit |
|---|---|---|
| `e17/session.sh` | `460deb8979bfbd30e154e2b97ab53a01bc9f4fe42d42c4197ae6a046c51b9a83` | yes |
| `e17/e17_vm.py` (PRT fix) | `6519f322a91dbdd8e9a93b5c9e9ae495965e25f478f3d8b8366c9678a3c1f008` | yes |
| `e17/pt_walk.py` (VRAM base fix) | `1e5e9afc5ae0bf37b79ad0a26a936a0e702bfaae586c8b08e509f58f5b30ce8e` | yes |
| `e17/pte_bits.json` | `bb36f068655c175bfacad170cbd426ab240d3952a29aafb38ef1bcb4d8cc7973` | yes |
| `e17/vmlists.json` | `20f645ff54a04be35fb4ae2d86ec176068aa32d3ac7c534dd8b6ef193d3a74d6` | yes |
| E13 `dispatch.py` | `87b3a433d3ed38c35aa41ebe67def53404f5c2521c5da7314d7109b3d1488578` | yes |
| E13 `dispatch.json` | `22553bc23ed3401ec2a2b18472650296120f70b178ef1402e04833f4e35b9276` | yes |
| E13 `info.py` | `17830e4eb2e816acbf3420cc4be330e0880b6f4a6c41bd40482ca018f4730eab` | yes |
| E13 `regs2.py` | `47c023a3fd045ecacc4f737b63ed2ec963dfd2db939d0dcc006db4bcf9583b3d` | yes |
| `run-b2.sh` | `e6752737c2ddea20b4b492cdb9b2be23e2d366555c75a61a3fb5424d4db25d44` | `scripts/run-b2.sh` |
| `chroot-run.sh` | `bbc532b2dcc71bfc30b90f8d80666fdbb145b95ae8736078bb97cee93370ae36` | `scripts/chroot-run.sh` |

`vkmembw` for L25 and L39 is the Linux build of visit 1's sources (same `vkmembw.c`, `windows.h` and
SPIR-V headers, hashes in visit 1's README), built again on the stick inside the reference root.
`sha256.txt` holds the SHA-256 of every other file in this directory.

## Timeline (UTC)

| UTC | Phase | Output |
|---|---|---|
| 12:55:32 | `e17 pre`, `e17 load`: amdgpu loaded under the event trace (90203 lines, 0 overruns) | `logs/out-pre.log`, `logs/out-load.log`, `out/e17/amdgpu-events-load.txt.gz` |
| 12:55:46 | `e17 base`: 412 VM registers with no client | `logs/out-base.log`, `out/e17/vmregs-base.txt` |
| 12:55:46 | `e17 hold`, attempt 1: failed, see below | `logs/out-hold-1-fail.log`, `out/e17/hold-1-fail/` |
| 12:57:09 | `e17 hold`, attempt 2: failed, see below | `logs/out-hold-2-fail.log`, `out/e17/hold-2-fail/` |
| 12:58:46 | `e17 hold`, attempt 3: mapped, walk failed, see below | `logs/out-hold-3-novrambase.log`, `out/e17/hold-3-novrambase/` |
| 13:00:23 | `e17 hold`, attempt 4: the measurement | `logs/out-hold.log`, `out/e17/hold/`, `out/e17/amdgpu-events-hold.txt` |
| 13:01 | `e17 extras`, `e17 info` | `logs/out-extras.log`, `logs/out-info.log`, `out/e17/` |
| 13:01:41 to 13:01:45 | `l25`: two RADV clients at once | `out/l25/` |
| 13:01 to 13:02 | `l38`: display audio, encoder dependencies | `out/l38/` |
| 13:02 | L39 follow-up: a manual clock request, then one `vkmembw` run | `out/l39b/`, `out/dmesg.txt` |
| 13:02:34 | Kernel log saved | `out/dmesg.txt` |

`out/e17/amdgpu-events-load.txt.gz` is the gzip of the 9173589-byte trace, SHA-256 of the
uncompressed file `7b2d032ad7115e0b906a6bf97d8d1384c15f1e0b566d7e9fcf993caf9c08bb9d`.

## E17 (wishlist L27): the first run of the kit on unit A

### Three failed attempts and two defects of the kit

1. Attempt 1: `e17_vm.py` stopped at `import dispatch`. The staged E13 helper directory on the stick
   held `info.py` and `regs2.py` but not `dispatch.py`. This was a staging defect of the batch, not of
   the kit.
2. Attempt 2: ten buffers mapped, then `GEM_VA MAP prt4k` failed with errno 22 (EINVAL). The kit gave the
   PRT rows the flags `R|W|P`. `amdgpu_gem_va_ioctl` admits only `AMDGPU_VM_DELAY_UPDATE` and
   `AMDGPU_VM_PAGE_PRT` for a sparse mapping (`prt_flags`, `ref/linux-src/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:797-798`).
   Fix: the PRT rows carry `AMDGPU_VM_PAGE_PRT` alone (flags `0x0010`).
3. Attempt 3: all twelve mappings held, but every `amdgpu_vm` read failed with errno 6 at `0x46ffe0000`.
   A directory entry and a VRAM leaf hold the system physical address, that is
   `vm_manager.vram_base_offset` (`gmc_v10_0.c:682`, `0x270000000` on unit A, M31, M85) plus the VRAM
   offset. The `amdgpu_vram` debugfs file takes the VRAM offset alone. Fix: `pt_walk.py` subtracts
   `E17_VRAM_BASE` from a VRAM address before the read. Attempt 4 ran with `E17_VRAM_BASE=0x270000000`.
   The logs do not print that variable. Every read of attempt 4 succeeded, and only that value maps
   `0x46ffe0000` into the 8 GiB VRAM file.

Both fixes are now in `experiments/E17-linux-reference-3`, each with a comment.

### Results of attempt 4

The holder mapped twelve regions and submitted nothing (`out/e17/hold/hold.txt`). The VM is 3 levels
below the root, block size 9, fragment size 9, root at `0x46ffe0001`
(`out/e17/hold/vm_pagetable_info.txt`).

Leaf entries (`out/e17/hold/pt-walk.txt`, decoded again on the PC in `analysis/check_pte.txt`):

| Region | Level | Entry | Decoded |
|---|---|---|---|
| `rw` GTT | 3 | `000000016db36077` | VALID SYSTEM SNOOPED EXECUTABLE READABLE WRITEABLE, NC |
| `ro` GTT | 3 | `000000016db3e027` | VALID SYSTEM SNOOPED READABLE, NC |
| `nx` GTT | 3 | `000000016d562067` | VALID SYSTEM SNOOPED READABLE WRITEABLE, NC |
| `noalloc` GTT | 3 | `0400000107c96067` | as `nx` plus NOALLOC (bit 58) |
| `k64` GTT | 3 | `000000010f1e0267` | as `nx`, FRAG 4 |
| `m2` GTT | 2 | `00400001092004e7` | as `nx`, PDE_PTE, FRAG 9 |
| `vram4k` VRAM | 3 | `000000046ffda061` | VALID READABLE WRITEABLE, NC |
| `vram2m` VRAM | 2 | `004000046fc004e1` | VALID READABLE WRITEABLE, PDE_PTE, FRAG 9 |
| `far1g` GTT | 3 | `0000000102ef6067` | as `nx` |
| `far512g` GTT | 3 | `00000001096e4067` | as `nx`, reached through root entry 1 |
| `prt4k` | 3 | `0088000000000006` | SYSTEM SNOOPED PRT (bit 51) LOG (bit 55), not VALID, address 0 |
| `prt2m` | 2 | `00c8000000000486` | SYSTEM SNOOPED PRT PDE_PTE LOG, FRAG 9, not VALID, address 0 |

Every directory entry is `<address> | 1`. Root entries 2 to 511 read `0040000000000000` (PDE_PTE alone).
`analysis/check_pte.txt` prints 14 claims: 14 confirmed, 0 refuted, 0 not covered.

The hypotheses of `experiments/E17-linux-reference-3/README.md`:

| # | Result | Evidence |
|---|---|---|
| H1 | CONFIRMED. The `prt4k` leaf is `0x0088000000000006`: PRT and LOG set, VALID clear, address 0 | `out/e17/hold/pt-walk.txt` |
| H2 | CONFIRMED. The `noalloc` leaf has bit 58 set. Its low flags (`0x67`) are those of `nx`, which differs only in the NOALLOC request | `pt-walk.txt`, `analysis/check_pte.txt` |
| H3 | CONFIRMED. All 45 entries read on the walk equal the value that the 38 `amdgpu_vm_set_ptes` events before the unmap describe | `analysis/h3-walk-vs-trace.txt` |
| H4 | CONFIRMED, the positive control. 10 of 10 buffers read their witness word at the physical address their own entry names | `out/e17/hold/pt-control.txt` |
| H5 | CONFIRMED. `m2` (GTT) and `vram2m` (VRAM) are each one PDE_PTE entry at level 2 with FRAG 9 | `pt-walk.txt` |
| H6 | CONFIRMED. `far512g` resolves through root entry 1 (`000000046ffd6001`), not inside root entry 0 | `pt-walk.txt`, `out/e17/hold/pt-scan.txt` |
| H7 | CONFIRMED. With the tables mapped and nothing submitted, `GCVM_CONTEXT1..15_PAGE_TABLE_BASE_ADDR_LO32/HI32` all read 0. `GCVM_CONTEXT0` reads `0x4_6fe00001` | `out/e17/hold/vmregs-hold.txt` |

The H3 check runs on the PC:
`python experiments/E17-linux-reference-3/offline/check_walk_vs_trace.py out/e17/amdgpu-events-hold.txt
out/e17/hold/pt-walk.txt --vram-base 0x270000000 --mc-base 0xF400000000 --until 331.0`. The holder
unmaps at 331.42 s of the trace, so the check stops at 331.0 s. In the trace, `pe` is an MC address
(VRAM at `0xF400000000`, the `VRAM:` line of `out/dmesg.txt`). The `addr` field of a directory entry and
of a VRAM leaf, and the bytes in VRAM, hold the system physical address (`0x270000000` + VRAM offset).
Example: the root at VRAM offset `0x1FFFE0000` is `pe=f5fffe0000` in the trace and `0x46ffe0000` in
every entry and in `GCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR` during L25.

`e17 info` (`out/e17/info.txt`) gives the same `AMDGPU_INFO` answers as E13 boot 4 (M46). Only the
usage counters, the time stamp and the sensor values differ.

## L25: two RADV clients on the gfx ring at once

`run-b2.sh l25` starts two `vkmembw --warmup-ms 4000 --iters 20 --placement local` processes at the same
time under the `amdgpu_vm_grab_id`, `amdgpu_vm_flush`, `amdgpu_cs_ioctl`, `amdgpu_sched_run_job` and
`amdgpu_vm_bo_map` events (`out/l25/trace.txt`, 1315 entries, none lost). Three times, one second apart,
it dumps every ring and reads the VM registers of both hubs.

- pasid 9 always gets VMID 1 with page directory `0x46ffe0001`: 190 grabs, `needs_flush=1` once.
- pasid 10 always gets VMID 2 with page directory `0x46ffdd001`: 189 grabs, `needs_flush=1` once.
- The trace has 379 `amdgpu_cs_ioctl` events on `gfx_0.0.0`. It has one `amdgpu_vm_flush` on the gfx ring
  per VMID, and 22 on `sdma0` for VMID 0 (`0x46fe00001`).
- All three register snapshots (`out/l25/vmregs-1.txt` to `-3.txt`) read `GCVM_CONTEXT1_PAGE_TABLE_BASE`
  `0x4_6ffe0001` and `GCVM_CONTEXT2_PAGE_TABLE_BASE` `0x4_6ffdd001`.
- Each gfx ring dump holds 8 submissions, 4 on VMID 1 and 4 on VMID 2, alternating, with no page table
  base write and no invalidation between them (`analysis/ring-gfx-1-decoded.txt` to `-3`, decoded by
  `experiments/E17-linux-reference-3/offline/ring_wrapper.py` with the kernel's `nvd.h` after
  `od -A x -t x4 -v`).
- Both clients pass their checks (`result ok`) at write 445.3 and 445.6, copy 397.3 and 397.2, read 394.0
  and 390.4 GB/s.

The ring dumps (21 files, 8204 bytes each) are in `out/l25/`. The compute and SDMA ring dumps stay in
that directory, and this file does not decode them.

## L38: display audio and the community encoder

`out/l38/audio.txt`:

- `snd_hda_intel` drives the GPU's audio function `01:00.1` (`1002:13ff`) from 14.9 s after boot. It binds
  to amdgpu's audio component at the amdgpu load (`bound 0000:01:00.0 (ops amdgpu_dm_audio_component_bind_ops [amdgpu])`).
- ALSA card 0 is `HD-Audio Generic` with two HDMI/DP inputs, `pcm=3` and `pcm=7`. The codec is
  `ATI R6xx HDMI` (vendor `0x1002aa01`).
- ELD on pin `0x3`: `monitor_present 1`, `eld_valid 1`, connection DisplayPort, one LPCM descriptor, 2
  channels, 32 to 192 kHz, 16/20/24 bit. Pin `0x5`: no monitor, ELD not valid.
- No sound played. The DCCG audio DTO registers were not read.

`out/l38/enc-deps.txt`: inside the reference root, `pkg-config` does not find `libva`. It reports
`libdrm` 2.4.124 and `vulkan` 1.4.309. `glslangValidator` is at `/usr/bin`. The shell's `command -v`
printed only that first name, so the file does not list the other tools. The encoder needs `libva`, so
the batch did not build it.

## L39 follow-up: the clock under amdgpu

A request to put amdgpu's performance level to manual failed. The kernel log has one line at 449.4 s:
`amdgpu: Failed to set performance level 2` (`out/dmesg.txt`). Level 2 is
`AMD_DPM_FORCED_LEVEL_MANUAL` (`kgd_pp_interface.h:57`), and `cyan_skillfish_ppt.c` has no
`set_performance_level` callback. The writes to `power_dpm_force_performance_level` and `pp_dpm_sclk`
answered EINVAL in the operator's shell. That shell output was not saved, so the kernel line is the
record. `out/l39b/sclk.txt` reads `1: 1500Mhz *` three times, and the `vkmembw` run after the request
(`out/l39b/local-1000.txt`, named for the clock it asked for) ran at 1500 MHz: write 445.4, copy 396.9,
read 393.5 GB/s.

## Kernel log warnings

The amdgpu load prints two `WARNING`s from `amdgpu_dm_hpd_init`: one in `dal_irq_service_ack`
(`irq_service.c:169`) and one in `dal_irq_service_set` (`irq_service.c:129`), both for HPD source 3.
`Failed to clear hpd(rx) source=N on init` follows for sources 3, 4, 5, 6, 9, 10, 11 and 12, each with
`called for non-implemented irq source`. The same lines are in visit 1 (`collect/dmesg-after.txt`) and in
E01 (`evidence/linux/2026-09-21-E01-recon/dmesg-full.txt`, lines 824 to 990). The kernel log of this
visit has no VM fault, no ring timeout and no GPU hang.

## Redaction

`REDACTED.txt` lists the removed items: a MAC address, the stick's USB serial number and the file
system UUID of the reference root in `out/dmesg.txt`. The monitor model name in `out/l38/audio.txt` is
not an identifier of the lab and stays. The community encoder source is GPL and is not in this
directory.
