# L1031 hang lane: SDMA0, compute and GFX queue resets, a compute spin kill, a CPU-polled GFX wait

Date: 2026-11-01, 00:34 to 00:48 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel
`6.18.52-0-lts`), GRUB entry `bc250.mode=network` (amdgpu blacklisted at boot). This lane ran in
Boot B, in the same amdgpu load as the init trace (`../2026-11-01-L1031-init-trace/`), about 25 min
after `modprobe amdgpu`. Wishlist rows: L48 (SDMA0 recovery), L43 (compute spin kill), L47 (GFX and
compute queue reset).

## The clock of this boot

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. At development PC time
`00:41:13Z`, `date -u` on the lab printed `01:41:41 UTC`. The UTC times in this file come from the
development PC: the stream prefixes of `kmsg-hang.txt` and `trace-hang.txt` and the "received" times
of the console blocks. Durations come from one clock only. The trace clock and the printk clock of
the kernel are different clocks: the trace stamps run about 12 ms after the printk stamps of the
same event, so this file never subtracts one from the other.

## What ran, in order

`scripts/h-arm.sh` armed the kprobes of `scripts/hang-probes.txt` in the trace instance `bchang`. The
development PC streamed `/dev/kmsg` and that instance's `trace_pipe` with `dd`, from 00:34:37Z to the
restart at 00:48:16Z. Each step ran one wrapper of `scripts/wrappers/`. `scripts/h-jobs.py` builds and
submits the jobs through the amdgpu CS ioctl. The start times in the table come from the monotonic
submit time of each job, set against the kernel log stamps of the same boot.

| UTC | Wrapper | Job | Output |
|---|---|---|---|
| 00:34:51 | `h48.sh` | SDMA `POLL_REGMEM` on a CPU flag, SDMA scheduler mask `0x1` (SDMA0 only), CPU release at 14 s | `l48-sdma0.txt` |
| 00:38:12 | `h43.sh` | compute spin, compute scheduler mask `0x1` | no job file: kernel oops (see below) |
| 00:41:13 | `alive.sh` | process state of the oopsed client | console only |
| 00:42:18 | `restore43.sh` | compute mask back to `0xff`, compute and SDMA controls | `l43-post-oops-i5.txt` |
| 00:42:57, 00:43:22 | `h43b.sh` | compute spin, kill thread failed (see below) | console only |
| 00:43:43 | `h43b.sh` | compute spin, kill at 3 s through `SQ_CMD` | `l43-kill.txt` |
| 00:43:58 | `h47g.sh` | GFX `WAIT_REG_MEM` on a CPU flag, CPU release at 14 s | `l47-gfx.txt` |
| 00:44:20 | `h47p.sh` | the same GFX wait, CPU release at 500 ms | `l47-gfx-post.txt` |
| 00:45:15 | `h47n.sh` | GFX NOP job, the GFX control after the two resets | `l47-gfx-nop.txt` |
| 00:46:17 | `h47u.sh` | the GFX wait of `h47p.sh` with the IB mapped `MTYPE_UC` (flags `0x8e`) | `l47-gfx-uc.txt` |

`out/boot-b/console-hang.txt` holds the console output of every wrapper, with the fence counters of
`amdgpu_fence_info` before and after each job and the post-reset controls. The controls are
`i5-submit.py` (a compute fill of 1024 bytes, `../2026-11-01-L1031-init-trace/scripts/`) and
`x1-sdma-raw.py` (a linear SDMA copy, `../2026-11-01-L1031-x1-sdma/scripts/`).

## L48: SDMA0 queue reset

- The SDMA0 job did not finish. `WAIT_CS` returned `ETIME` (errno 62) at 10.115 s, the marker stayed
  0, and the tool exited 2. This is the expected result for a hung job, not a successful job.
- `kmsg-hang.txt` at 00:35:01.93Z: "ring sdma0 timeout, signaled seq=217, emitted seq=218", then
  "Starting sdma0 ring reset", "Ring sdma0 reset succeeded" and "device wedged, but recovered through
  reset".
- `trace-hang.txt`: `amdgpu_job_timedout` calls `sdma_v5_0_reset_queue`, which calls
  `amdgpu_sdma_reset_engine` with instance 0. That calls `sdma_v5_0_stop_queue`,
  `sdma_v5_0_soft_reset_engine` and `sdma_v5_0_restore_queue`, all with return value 0. The queue reset
  takes 1.226 ms on the trace clock. The whole `amdgpu_job_timedout` call takes 4.9 ms.
- The probes on `amdgpu_device_gpu_recover` and `nv_asic_reset` never fired in this lane. Every reset
  in this file is a queue reset, not a full GPU recovery and not an ASIC reset.
- After the reset, the SDMA0 fence went from `0xcf` to `0xdc` and SDMA1 stayed at `0x159`. The SDMA0
  copy control passed at 4096 bytes and at 1 MiB (destination copied, guard intact). With the mask
  back to `0x3`, the compute control passed.

This extends the SDMA1 queue reset of E29 (`../2026-09-24-E29-sdma-reset/`) to SDMA0 on this kernel.

## Compute scheduler mask 0x1: kernel oops

`h43.sh` wrote `0x1` to `amdgpu_compute_sched_mask`. This leaves only compute ring 0. amdgpu gives
ring 0 high priority when the device has more than one compute ring
(`amdgpu_gfx_is_high_priority_compute_queue` in `amdgpu_gfx.c`). The spin job then failed in the CS
ioctl:

- `kmsg-hang.txt` at 00:38:12.23Z: the other compute schedulers are "not ready, skipping", then
  "BUG: kernel NULL pointer dereference" with RIP `drm_sched_job_arm+0x2b/0x50 [gpu_sched]`, called
  from `amdgpu_cs_ioctl`.
- The process (PID 6364) stayed in state D in `drm_exec_lock_obj`, called from
  `amdgpu_gem_object_close` at exit. It was still in state D at 00:41Z and at 00:48Z.
- This was not a GPU hang. With the mask back to `0xff` at 00:42:18Z, the compute control
  (`l43-post-oops-i5.txt`, 256 of 256 dwords hold the fill pattern) and the SDMA copy control passed.

The later runs left the compute mask at `0xff`.

## Compute queue reset through the job timeout (two spin runs)

The first two `h43b.sh` runs did not kill the spin. The kill thread of that revision of `h-jobs.py`
read `amdgpu_ring_comp_1.0.0` from debugfs and got `EINVAL`, and the job ran on ring `comp_1.1.0`.
The job timeout then reset the queue:

- `kmsg-hang.txt` at 00:43:07.31Z and 00:43:31.86Z: "ring comp_1.1.0 timeout", "Ring comp_1.1.0 reset
  succeeded".
- `trace-hang.txt`: `gfx_v10_0_reset_kcq` with return value 0, 0.179 ms and 0.172 ms. The
  `amdgpu_job_timedout` calls take 14.8 ms and 15.0 ms.
- `WAIT_CS` returned at 10.436 s and at about 10.17 s with status 0, and the tool printed `rc 0`. The
  fence of a reset job signals with an error, so status 0 here does not mean that the spin finished.
- The compute control after each run passed.

## L43: compute spin kill through SQ_CMD

`l43-kill.txt` (00:43:43Z): the spin shader (`s_branch -1`, `bf82ffff`) runs on `comp_1.1.0`, the only
busy compute ring (fence 18 signalled, 19 emitted). The ring reads rptr = wptr = 768, and the job runs
in VMID 3. At 3.004 s the tool wrote `SQ_CMD` = `0x30000093` (kill the waves of VMID 3).

- `WAIT_CS` returned status 0. The wait took 3.002 s from submit, so 3.002 s is the whole job time,
  not the kill latency.
- `GRBM_STATUS` read `0xA0403028` while the spin ran and `0x00003028` at 3.204 s, after the kill.
- No job timeout and no queue reset followed. The compute control passed and the compute fence moved
  on to `0x13`.
- The log stamps resolve milliseconds and come from two threads, so this run does not give the drain
  time of the kill to 10 ms.

The no-retry `MEMVIOL` arm of L43 did not run.

## L47: GFX queue reset and the CPU-polled GFX wait

- `l47-gfx.txt`: the GFX `WAIT_REG_MEM` job (default mapping, flags `0xe`, CPU release at 14 s)
  returned `ETIME` at 10.400 s, tool exit 2. `kmsg-hang.txt` at 00:44:08.73Z: "Ring gfx_0.0.0 reset
  succeeded". `gfx_v10_0_reset_kgq` returned 0 after 0.275 ms. The compute and SDMA controls passed.
- `l47-gfx-post.txt`: the same job with a CPU release at 500 ms. The CPU wrote the flag at 0.501 s,
  but the job was still busy at 10.495 s. `CP_STAT` read `0x88038200`, and the tool exited 3. A second
  GFX queue reset followed at 00:44:30.74Z (`gfx_v10_0_reset_kgq`, 0.283 ms, return value 0).
- `l47-gfx-nop.txt`: a GFX NOP job completed at once, and the GFX fence moved to `0x1b`. This is the
  GFX control after the two resets. It is a NOP job, not the new-context rendered pattern that L47
  asks for.
- `l47-gfx-uc.txt`: the IB of the 500 ms job mapped `MTYPE_UC` (flags `0x8e`). The wait returned at
  0.500 s with status 0, tool exit 0.

The default mapping did not see the CPU write within 10 s. The `MTYPE_UC` mapping saw it at the
release time. A cached copy of the flag on the GPU side agrees with this result, but this lane did not
measure the cache.

## Files

- `out/boot-b/l48-sdma0.txt`, `l43-kill.txt`, `l47-gfx.txt`, `l47-gfx-post.txt`, `l47-gfx-nop.txt`,
  `l47-gfx-uc.txt`, `l43-post-oops-i5.txt`: job files as the lab wrote them (`/root/hang-out/`).
- `out/boot-b/kmsg-hang.txt`: the kernel log stream, with development PC UTC prefixes. The stream
  replays the log of the whole boot from its start, so it also holds the init of Boot B.
- `out/boot-b/trace-hang.txt`: the kprobe stream of the instance `bchang`.
- `out/boot-b/console-hang.txt`: the console output of the wrappers, from the session tool log of the
  development PC (11 blocks).
- `scripts/`: `h-arm.sh`, `hang-probes.txt`, `h-jobs.py`, `hang.json`, `packets.json` and
  `gen-hang-tables.py` (writes `hang.json` from the 6.18.52 sources). `scripts/wrappers/` holds the
  wrapper scripts, taken from the commands of the session tool log.

`scripts/h-jobs.py` is the last revision, written at 00:46:09Z before the `MTYPE_UC` run. The session
changed the file five times between 00:42:28Z and 00:46:09Z: the busy-ring search of the kill, the NOP
job and the `MTYPE_UC` mapping. The earlier runs used the earlier revisions, which are not kept.

## Limits

- All results come from one amdgpu load on one kernel (`6.18.52-0-lts`) and one firmware set.
- No full GPU recovery ran. These results do not change M53: a full amdgpu GPU reset is still not
  available on this part.
- The GFX control after reset is a NOP job, not a rendered pattern in a new context.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. `sha256.txt` holds the SHA-256 of every file in
this directory except itself.
