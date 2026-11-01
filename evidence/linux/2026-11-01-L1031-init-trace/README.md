# L1031 init trace: amdgpu bring-up states under kprobes, post-init reads, traced compute submit

Date: 2026-11-01, Boot B, `modprobe amdgpu` at about 00:13:17 UTC. Two Boot A reads of 2026-10-31,
about 23:56 to 00:01 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`), GRUB
entry `bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand once per boot). Wishlist rows:
L16 (init stage states), L3 (PSP commands, firmware), L4 (rings and MQDs), L5 (traced dispatch),
L10 and L31 (SMU surface), L32 (power gating masks), L34 (scheduler state), L45 (occupancy inputs).

## The clock of these boots

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead of the development PC.
`out/boot-b/modprobe-start.txt` prints `01:13:45 UTC` for the start of `modprobe`, which is about
00:13:17Z. The `utc=` and `date -u` stamps of the lab files carry this skewed clock. The trace
stamps are seconds since boot. Boot A started after the AC cycle at 23:48:10Z (see
`../2026-11-01-L1031-l6/`), and its `modprobe` ran at about 23:56Z. The Boot A times that the campaign
notes give (00:33:57Z, 00:56:49Z) mix the two clocks and are not used here.

## What ran

Boot B, in this order:

1. `scripts/i0-pull-module.sh` and `scripts/ko_offsets.py`: the offsets of `rmmio` and of the
   register access bodies, read from the running `amdgpu.ko` and its BTF (`scripts/i0/offsets.json`).
   `scripts/i0/sha256.txt` holds the hash of that module. The module itself is not in this directory.
2. `scripts/gen_probes.py` and `scripts/i1-arm.sh`: 33 kprobes in the trace instance `bcinit`
   (`scripts/i0/probes.txt`). Each stage probe reads 19 registers through `rmmio`. The write probe
   `st_w` on `amdgpu_device_wreg` passes only the 14 registers of `scripts/i0/filters.txt`.
3. `scripts/i2-modprobe-trace.sh`: `modprobe amdgpu`, exit 0. The trace is `out/boot-b/init-trace.txt`
   (390 events). `kprobe_profile.txt` gives the hit counts: `st_w` 13745 hits with 101 events kept by
   the filter, `db32` 116, `ib_sched` 27, `psp_cmd` 11. `copy_buf` had 0 hits. `st_w` and `db32` each
   missed one hit.
4. `scripts/i3-post-init.sh` and `scripts/i4-per-se.sh`: post-init reads (L3, L4, L32, L34, L45).
5. `scripts/i5-submit.sh`: one compute dispatch under trace (L5).
6. `scripts/i7-smu.sh`: the SMU surface (L10, L31).

The paging step `i6` of the same boot is in `../2026-11-01-L1031-paging-l26/`.

Boot A: `scripts/boot-a/l5-dispatch.sh` and `l5-dispatch2.sh` ran the same compute dispatch without a
trace (`out/boot-a/l5-dispatch.txt`, PASS). `out/boot-a/l45-*` are the L45 reads of that boot.

## Init stage states (L16)

The kprobe on `amdgpu_device_wreg` fires at the entry of the function. The register state that a
write event carries is therefore the state before that write. "First seen" below means the first
event that carries the new value. The change happened between that event and the event before it.

Register names from `tools/regcalc`: `psp64` is `MP0_SMN_C2PMSG_64` (MP0 header), `f0` and `f1` are
`SDMA0_F32_CNTL` and `SDMA1_F32_CNTL`, `spi` is `SPI_PG_ENABLE_STATIC_WGP_MASK`, `pgc` is
`RLC_PG_CNTL`, `rlc` is `RLC_CNTL`, `rlcst` is `RLC_STAT`, `gpm` is `RLC_GPM_STAT` and `sched` is
`RLC_CP_SCHEDULERS`.

| Trace time (s) | Event | State change, first seen |
|---|---|---|
| 498.642924 | first `amdgpu_device_ip_block_add` | start state: `spi` `0xFFFF`, `pgc` 0, `rlc` 0, `gpm` `0xF40016`, `sched` `0x58504840`, `f0` 1, `f1` 1, `psp64` `0x80000000` |
| 498.689773 | PSP command `SETUP_TMR` (id 5) | `psp64` `0x80000000` -> `0x80020000` |
| 498.691980 | PSP `LOAD_IP_FW` of `SDMA1` (type 10) | `f0` 1 -> 0, after the `SDMA0` load (type 9) |
| 498.694009 | PSP `LOAD_IP_FW` of `CP_CE` (type 3) | `f1` 1 -> 0, after the `SDMA1` load |
| 498.726396 | `smu_hw_init` entry | `rlc` 0 -> 1, `rlcst` 0 -> `0x25`, `gpm` -> `0xB40017`, after the `RLC_G` load (type 8) |
| 498.726473 | first SMU message, write of the message register | `spi` `0xFFFF` -> `0xFFE7` (the message entry at 498.726416 still shows `0xFFFF`) |
| 498.726520 | second SMU message entry | `spi` `0xFFE7` -> `0x7`, `pgc` 0 -> `0x8`, `rlcst` -> 0, `gpm` -> `0xB40016` |
| 499.022453 | `rlc_resume` entry | host writes `RLC_CNTL` = 0, `RLC_PG_CNTL` = 0, `RLC_CNTL` = 1 |
| 499.022699 | host write `RLC_CP_SCHEDULERS` = `0x585048C8` | right after `rlc_resume` returns |
| 499.023165 | host write `CP_MEC_CNTL` = 0 | MEC released |
| 499.024082 | host write `CP_ME_CNTL` = 0 | ME released, after the first ring test |
| 500.035801 | a later write event | `sched` `0x585048C8` -> `0x5850C0C8` |

- All 11 PSP commands returned 0. Their command IDs and the first six dwords of each command are in
  the `psp_cmd` events.
- The filtered trace holds no host write to `SDMA0_F32_CNTL`, `SDMA1_F32_CNTL` or `RLC_CNTL` before
  the PSP loads end. This agrees with M35: the PSP starts what it loads.
- The SPI mask is not in the write filter. This trace therefore does not show who wrote it. The full
  E03 trace (M17) holds no host write to that register. The change comes during the first SMU message
  call, after the `RLC_G` load. That the RLC firmware makes the change is an inference from this order.
- No filtered host write to `RLC_CP_SCHEDULERS` precedes the change to `0x5850C0C8`.
- `CC_GC_SHADER_ARRAY_CONFIG` (`0xFFF80000`) and `RLC_PG_ALWAYS_ON_WGP_MASK` (`0x3`) do not change in
  any event.

## Post-init reads (L3, L4, L32, L34, L45)

- L3: `l3-firmware-info.txt` (34 lines of `amdgpu_firmware_info`) and `l3-firmware-hashes.txt` (the
  SHA-256 of each `.bin.zst` on the stick and of its decompressed file). `mec` and `mec2` are the same
  file. The PSP part of L3 is the 11 command events above. No raw PSP ring frame and no command or
  response buffer dump exists.
- L4: `l4-amdgpu_ring_*` (12 rings) and `l4-amdgpu_mqd_*.hex` (10 MQDs: GFX, KIQ, 8 compute). The
  KIQ ring files are gzip copies. These files are not decoded here.
- L32: `l32-pg-broadcast.txt` reads `SPI_PG_ENABLE_STATIC_WGP_MASK` `0x7`, `CC_GC_SHADER_ARRAY_CONFIG`
  `0xFFF80000`, `RLC_PG_ALWAYS_ON_WGP_MASK` `0x3`, `RLC_LB_ALWAYS_ACTIVE_WGP_MASK` `0x1`,
  `RLC_MAX_PG_WGP` `0xC`, `RLC_STATIC_PG_STATUS` `0xFFE7`, `RLC_PG_CNTL` 0, `RLC_CGCG_CGLS_CTRL` 0 and
  `RLC_CGTT_MGCG_OVERRIDE` `0xFFFFFFFF`, at the stock 24 CU configuration.
- `l32-pg-per-sa.txt` and `l34-sched-per-sa.txt` read the same tables through the bank-select offset
  of debugfs `amdgpu_regs` (`scripts/regread_bank.py`). Every bank reads the broadcast value. This read
  has no positive control: no register of the tables differs between banks, and `GRBM_GFX_INDEX` reads
  0 in every column, the broadcast column included. So these files do not show that the bank select
  took effect.
- L34: `l34-scheduler-state.txt` reads `RLC_CP_SCHEDULERS` `0x5850C0C8`, `RLC_CNTL` 1,
  `RLC_GPM_STAT` `0x00B40016`, `CP_ME_CNTL` 0, `CP_MEC_CNTL` 0 and `GRBM_STATUS` `0x00003028`.
- L45: `amdgpu_gpu_info` does not exist on this kernel (`l45-occupancy.txt`, Boot A
  `l45-gpu-info.txt`). `l45-occupancy.txt` holds `ip_discovery` and the KFD topology. These files do
  not give the VGPR allocation or occupancy answer of L45.

## L5: traced compute dispatch

`i5-submit-result.txt`: one workgroup of 64 threads fills 1024 bytes with `0x22222222`, and 256 of 256
dwords match. The IB is the libdrm compute test sequence (`scripts/dispatch.json`). `i5-trace.txt`
holds the `ib_sched` events with the first IB dwords and the doorbell writes. The `i5-ring-*` files are
ring dumps before and after the submit. The separate SDMA write-linear and GFX NOP sequence of L5 did
not run.

## L10 and L31: SMU surface

`l31-pm_info.txt`, `l31-pp-dpm.txt`, `l31-hwmon.txt`, `l31-gpu_metrics-raw.hex` and
`l10-fan-thermal.txt`. `pp_features` does not exist (`l10-pp_features.txt`). The amdgpu hwmon device
has no fan files. The `gpu_metrics` bytes and the decoded reads come from separate reads. They are not
one raw SMU table read at one time.

## Files not copied

- The Boot A L4 rings: Boot B has the same set after a traced init.
- Boot A `l5-sdma-control.txt`: the script was not on the lab, so the file holds only an error line.
- Boot A `l32-pg-masks.txt`: despite its name, this is the L49 SMN table read through debugfs
  `amdgpu_regs`. It is in `../2026-11-01-L1031-visit/out/boot-a/l49-smn-through-debugfs.txt`.
- The empty `l3-firmware-hashes.txt` of the init output directory. The copy here comes from the
  userland output directory of the same boot.

The init outputs of Boot B exist in three pulled copies on the development PC (`bootB-init`,
`bootB-secure`, `bootB-final`). Their init files are the same. This directory copies them from
`bootB-secure`, the copy that also holds the X1 and DCN outputs of the boot.

## Limits

- One traced init on one kernel and one firmware set. The 101 kept write events cover only the 14
  filtered registers.
- The 40 CU constants-stage arm of L32 did not run. The second-load comparison of L34 needs a module
  unload (L17), which did not run.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. `sha256.txt` holds the SHA-256 of every file in
this directory except itself.
