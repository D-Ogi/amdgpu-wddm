# E48: GPU memory bandwidth on unit A through Vulkan compute

Date: 2026-10-01, 13:03-13:06Z. Unit A, Windows 11 Pro build 22631, kernel driver 0.7.193.1 with DPM on (ceiling
2000 MHz), the registered Vulkan ICD ("AMD BC-250 (RADV GFX1013)", driver 0x06802063), desktop composed on the GPU.

Question: does the GPU reach DRAM at full speed under our driver? Linux's amdgpu reports "mclk/fclk 450 MHz" for
this part (M90). If the memory or fabric ran in a low state, that would be a large lever for the GPU-bound presets.
High with RT does not follow the shader clock (M775, K46).

Method: `vkmembw` (bc250-win `tools/win/vkmembw`, binary SHA-256 BFA13D6E20D3E86A...) through `run-membw.ps1` in
one SSH session, with the DPM sampler's CLI (`bc250kmd_cli dpm 60 250`, `*-dpm.txt`, local lab time = UTC+2) beside
it. Buffers of 256 MiB, 16-byte elements, 4096 groups of 256 invocations, timed by GPU timestamps (period 10 ns).
Every run checks its copy (first and last MiB) and its read (per-lane sum against the CPU's).

- Positive control (development PC, RTX 4090, `positive-control-rtx4090.txt`): write 914, copy 896, read 922 GB/s
  local against 1008 GB/s nominal; the host-visible placement across PCIe 10-11 GB/s.
- Negative control on unit A (`a5`, one element short): both checks fail, exit 1.

Results on unit A (median of the timed dispatches; GB = 10^9 bytes, copy counts read plus write):

| run | placement | clock during the timed dispatches | write | copy | read |
|---|---|---|---|---|---|
| a2 | local (type 0, DEVICE_LOCAL) | 2000 MHz after the 2 s warmup (a3's samples) | 369.5 | 388.8 | 385.5 |
| a2 | host (type 2, HOST_VISIBLE + HOST_COHERENT, heap 0) | 2000 MHz | 371.3 | 388.0 | 376.5 |
| a3 | local | 2000 MHz (raised 1.3 s into the warmup) | 370.9 | 387.9 | 385.2 |
| a4 | local, no warmup | 1000 MHz in all 60 samples | 375.1 | 386.1 | 373.1 |

- The timestamps and the CPU time per command buffer agree to within 4-10 %, the difference being submission and
  barrier overhead.
- Copy at 386-389 GB/s is 86-87 % of the 448 GB/s that 256-bit GDDR6 at 14 Gbps would give. That figure is the
  configuration commonly cited for this board, not measured here.
- Reading: DRAM bandwidth is the same at 1000 and 2000 MHz shader clock. Under our driver it is reached from both
  the local and the host-coherent memory type, and the memory is not in a low state. A frame that does not follow
  the shader clock is therefore bound by memory latency or by the work itself, not by a throttled memory clock.
- Open: whether M90's "450 MHz" under Linux is a different unit of the same state or a lower state there. The same
  binary's Linux build under amdgpu would decide; `docs/linux-session-wishlist.md` has the row.

Facts: M776.
