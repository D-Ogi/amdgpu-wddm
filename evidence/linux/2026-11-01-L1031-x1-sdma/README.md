# L1031 X1: SDMA linear copies from 4 KiB to 4 MiB with the stock SDMA firmware

Date: 2026-11-01, Boot B, about 00:18 to 00:19 UTC. Two Boot A controls of the same night, about
00:01 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel `6.18.52-0-lts`), GRUB entry
`bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand once per boot). Firmware: the stock
`cyan_skillfish2_sdma.bin` and `cyan_skillfish2_sdma1.bin` of the stick (hashes in
`../2026-11-01-L1031-init-trace/out/boot-b/l3-firmware-hashes.txt`). No other SDMA firmware was loaded.

## Question

A community report (akandr/bc250-rocm) says that the stock SDMA firmware of this part never completes
a copy above 16384 bytes. X1 tests that report with one SDMA `COPY_LINEAR` packet per submission, the
packet shape of `sdma_v5_0_emit_copy_buffer`.

## The clock

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. At development PC time
`00:18:15Z`, `date -u` on the lab printed `01:18:43 UTC`. The UTC times here come from the development
PC.

## Method

`scripts/x1-sdma-raw.py` submits through the raw amdgpu CS ioctl. Its packet fields come from
`third_party/linux-amdgpu/navi10_sdma_pkt_open.h` (`--gen`, written to `scripts/x1-packets.json`).
For each size the tool writes a pattern to the source, submits one copy, waits up to 5 s, and checks
the destination and a 64-byte guard after it. It stops at the first busy job or mismatch. Sizes:
4096, 16384, 16388, 20480, 65536, 262144, 1048576 and 4194304 bytes.

The SDMA scheduler mask in debugfs (`amdgpu_sdma_sched_mask`) selects the engine. Mask `0x1` leaves
only `sdma0`, mask `0x2` only `sdma1`. The fence counters of `amdgpu_fence_info` before and after each
masked run show which engine ran the copies.

## Results (`out/boot-b/`)

| File | Route | SDMA mask | Result |
|---|---|---|---|
| `x1-ring0.txt` | GTT to GTT | `0x3` | 8 of 8 sizes copied, guard intact |
| `x1-mask0x1.txt` | GTT to GTT | `0x1` | 8 of 8 |
| `x1-mask0x2.txt` | GTT to GTT | `0x2` | 8 of 8 |
| `x1-gtt2vram-mask0x1.txt`, `x1-gtt2vram-mask0x2.txt` | GTT to VRAM | `0x1`, `0x2` | 8 of 8 each |
| `x1-vram2gtt-mask0x1.txt`, `x1-vram2gtt-mask0x2.txt` | VRAM to GTT | `0x1`, `0x2` | 8 of 8 each |
| `x1-vram2vram-mask0x1.txt`, `x1-vram2vram-mask0x2.txt` | VRAM to VRAM | `0x1`, `0x2` | 8 of 8 each |

Every copy completed at once (`wait returned after 0.000 s`) and every tool run exited 0.

Fence counters of the two GTT to GTT masked runs (`console-x1.txt`): with mask `0x1` the `sdma0`
fence went from `0x2a` to `0x41` and the `sdma1` fence stayed at `0x27`. With mask `0x2` the `sdma1`
fence went from `0x27` to `0x3e` and the `sdma0` fence stayed at `0x41`. So both engines completed
copies of all eight sizes, the 16388-byte size included. These counters exist only in the console
output that the development PC kept, not in a lab file.

While the mask was `0x2`, a kernel job on `sdma0` failed to start at 846.11 s. See
`../2026-11-01-L1031-paging-l26/README.md`. The user copies of that run passed.

## Boot A controls (`out/boot-a/`)

`x1-sdma0-control.txt` and `x1-sdma1-control.txt`: one `POLL_REGMEM` job per engine with the CPU flag
already set, made with the E29 tool `sdma-reset-probe.py`
(`../2026-09-24-E29-sdma-reset/`). Both completed and wrote the marker. These controls show that
each engine runs a job. They do not test a copy size.

## Files

- `out/boot-b/x1-*.txt`: the nine runs, from the lab output directory of Boot B.
- `out/boot-b/console-x1.txt`: two console blocks from the session tool log of the development PC.
- `out/boot-a/`: the two Boot A controls.
- `scripts/x1-sdma-raw.py`, `scripts/x1-packets.json`: the test that made the Boot B files.
- `scripts/boot-a/x1-sdma.sh`: the Boot A control commands, taken from the session tool log.
- `scripts/x1-sdma.sh` and `scripts/x1-sdma-copy.py`: a Vulkan form of the same test. They are on the
  stick, but no output of them exists from this campaign.

## Limits

- Sequential single linear copies, one per submission, on one kernel and one firmware set.
- No concurrent copies, no tiled copies, no copy above 4 MiB and no other SDMA firmware.
- This result shows that the report does not hold for these copies on unit A. It does not test the
  workload of the report.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. `sha256.txt` holds the SHA-256 of every file in
this directory except itself.
