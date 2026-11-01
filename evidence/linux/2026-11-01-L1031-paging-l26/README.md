# L1031 L26: a paging round trip of a VRAM buffer through SDMA, with ring snapshots

Date: 2026-11-01. Boot C, about 00:56:30 UTC (the main run). Boot B, about 00:18:04 UTC (an eviction
with almost no load). Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`), GRUB entry
`bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand once per boot). Boot C loaded amdgpu
with `dcdebugmask=0x1` at about 00:51:10Z for the DCN single-pipe reads
(`../2026-11-01-L1031-display/`). Wishlist row: L26 (real paging).

## The clock

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. The UTC times here come
from the development PC: the stream prefixes of the kernel log and the times at which the console
output arrived. The trace stamps and the `monotonic` stamps of the job tool are seconds since boot.

## Boot C: the round trip (`out/boot-c/`)

`scripts/l26-run.sh` arms five kprobes (`amdgpu_copy_buffer`, its return, `amdgpu_ib_schedule` and the
two doorbell writes) and three amdgpu trace events (`amdgpu_bo_move`, `amdgpu_sched_run_job`,
`amdgpu_bo_create`) in the trace instance `l26`. It then runs `scripts/l26-migrate.py`:

1. Allocate a 16 MiB VRAM buffer object that the CPU can map, and write a pattern through the CPU
   mapping.
2. Copy the last 4 KiB of the buffer to a 4 KiB destination buffer with an SDMA job, and check the
   copy (the "resident" sample).
3. Call `amdgpu_evict_vram` through debugfs. It returns 0 after 0.004 s.
4. Copy the same 4 KiB again (the "restore" sample). The validation of this job makes TTM move the
   buffer back to VRAM.
5. Check the copy and the whole buffer through the CPU mapping.
6. Dump the `sdma0` and `sdma1` rings (`ring-sdma*-<n>-<stage>.bin`) at the start and after each stage.

Result (`l26-console.txt`): both copies match, the whole buffer matches through the CPU after the
round trip, `L26_RESULT rc 0`, exit 0.

`l26-trace.txt`, with TTM placements 1 = GTT and 2 = VRAM:

| Event | Count |
|---|---|
| `amdgpu_bo_move` VRAM -> GTT | 1 of 16 MiB, 13 of 4 KiB |
| `amdgpu_bo_move` GTT -> VRAM | 1 of 16 MiB, 7 of 4 KiB |
| `amdgpu_copy_buffer` | 36, all on one ring: 16 copies of 2 MiB (8 each way) and 20 copies of 4 KiB |
| `amdgpu_sched_run_job` | 75 on `sdma0`, 19 on `sdma1` |

- TTM moves the 16 MiB buffer in 2 MiB pieces through a GTT window, each piece one
  `amdgpu_copy_buffer` call. All 36 calls return 0.
- The kernel copies run on `sdma0`, the TTM buffer ring of this kernel. The job tool's own copies ran
  on `sdma1`: its rptr moves (1824 to 1888 to 2032) while the `sdma0` rptr stays at 368 during the
  resident sample.
- `decoded-ring-sdma0-3-restored.txt` decodes the last `sdma0` snapshot around three IBs of
  `l26-trace.txt`. The command is in the header of that file. It was made at landing from
  `ring-sdma0-3-restored.bin` with `scripts/sdma-ring-decode.py`. Around each `INDIRECT` it shows the
  frame that amdgpu writes around an IB: `GCR_REQ` (`c3a10000`), `COND_EXE` (address `0x401700`, reference 1, count 12),
  `INDIRECT` (length 8, VMID 0), `FENCE` (`0x00030005`, address `0x4016c0`), `TRAP` and `NOP`.

## Boot B: eviction with almost no VRAM use (`out/boot-b/`)

`scripts/i6-paging.sh` and `scripts/i6-paging.py` called `amdgpu_evict_vram` under fbcon. It returned
0 after 0.001 s. The trace holds 6 calls of `amdgpu_copy_buffer`, each of 4096 bytes, all with return
value 0. The VRAM manager was almost empty, so this run is no paging load. The Boot C run replaces it
as the L26 answer.

## A side observation of Boot B: SDMA mask 0x2 and the kernel's copy ring

During the X1 runs of Boot B (`../2026-11-01-L1031-x1-sdma/`) the SDMA scheduler mask was `0x2`, so
`sdma0` was not ready for new jobs. At 846.11 s the kernel log has "couldn't schedule ib on ring
<sdma0>" and "Error scheduling IBs (-22) in ring(sdma0)"
(`../2026-11-01-L1031-init-trace/out/boot-b/kmsg-bootB.txt`). All eight user copies of that run passed
on `sdma1`. The failed job was a kernel job on `sdma0`. This run does not show which kernel job it was.

## Files

- `out/boot-c/l26-console.txt`, `l26-trace.txt`, `ring-sdma0-*.bin`, `ring-sdma1-*.bin` (raw ring
  bytes, 8204 bytes each).
- `out/boot-c/decoded-ring-sdma0-3-restored.txt`: made at landing, see above.
- `out/boot-b/i6-*`: the Boot B eviction, its trace and ring dumps.
- `scripts/`: `l26-run.sh`, `l26-migrate.py`, `sdma-ring-decode.py`, `i6-paging.sh`, `i6-paging.py`.
  `l26-migrate.py` imports `x1-sdma-raw.py` and its `x1-packets.json`
  (`../2026-11-01-L1031-x1-sdma/scripts/`) and the `dispatch` module of E29
  (`../2026-09-24-E29-sdma-reset/sources/dispatch.py`).

## Limits

- One round trip of one 16 MiB buffer, with one forced eviction. This is not a memory pressure test.
- The kernel copies ran on `sdma0` and the user jobs on `sdma1`. The experiment did not use one
  engine for every operation.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. `sha256.txt` holds the SHA-256 of every file in
this directory except itself.
