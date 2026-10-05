# vkmembw: GPU memory bandwidth through Vulkan compute

One C file over core Vulkan 1.1 (`vulkan-1.dll` loaded at run time) and three compute shaders. It measures
write, copy and read bandwidth on buffers of `--mib` MiB with 16-byte elements and grid-stride loops. Each
dispatch is timed between two bottom-of-pipe timestamps, and the CPU time of each command buffer is printed beside
it as a cross-check of the timestamp period. A run is only a measurement if its checks pass:

- **copy**: the first and the last MiB of the destination are compared word by word with the pattern.
- **read**: the per-lane sum (mod 2^32) of all per-invocation results must equal the pattern's sum computed on
  the CPU. A sum, not an XOR: over a power-of-two range the pattern's XOR is 0, which is also what an idle kernel
  leaves in a zeroed buffer.

`--negative-control` runs the timed copy and read one element short. Both checks must then fail and the exit code
is 1. `--warmup-ms` (default 2000) keeps the GPU busy with copies first, so a load-driven clock governor has
raised the clock before the timed dispatches; 0 measures at whatever clock the governor holds.
`--placement local|host|both` picks a DEVICE_LOCAL type without HOST_VISIBLE, a HOST_VISIBLE|HOST_COHERENT type
without DEVICE_LOCAL, or both in turn.

    pwsh -File build.ps1 -Kits <BC250_ROOT>\toolchain\nuget      -> <BC250_ROOT>\scratch\build\vkmembw\vkmembw.exe
    vkmembw.exe [--device N] [--mib N] [--iters N] [--groups N] [--warmup-ms N] [--placement local|host|both]
                [--negative-control]

Exit codes: 0 every check passed, 1 a check failed, 2 usage, 3 Vulkan or system error. The build embeds the
SPIR-V, compiled by glslangValidator `-V`, as C arrays. It also keeps a replaced binary under `retained\` by its
hash.

Positive control: on an RTX 4090 the tool reads about 900 GB/s from local memory, against 1008 GB/s nominal.
Unit A's results are in `evidence/windows/2026-10-01-E48-gpu-memory-bandwidth/`.
