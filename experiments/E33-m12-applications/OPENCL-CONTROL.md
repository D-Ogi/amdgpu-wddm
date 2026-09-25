# OpenCL integer and local-memory control

Status: source compiled with MSVC19.44, C11, /W4 /WX on2026-09-25.
Runtime validation is pending. This is an initial ADR0016 content control,
not the OpenCL conformance suite or M12 acceptance.

## Hypothesis and oracle

The selected GPU executes clvk/clspv kernels with exact unsigned32-bit results,
including an in-order dependency between dispatches and local-memory barriers.
`opencl_content_control.c` transforms4096 deterministic words, then reduces
64 groups of64 words. The CPU computes each transformed word and each group sum
independently of the GPU output. Arithmetic wraps modulo2^32 on both sides.
Every output word is compared; mismatch output is bounded, but the count is not.

The command requires an expected GPU-name substring and exactly one matching
GPU. CPU devices are excluded. The external runner must additionally record
actual loaded OpenCL/Vulkan DLL paths and hashes; a device name alone does not
prove use of the intended ICD.

## Procedure

1. Finish the active Vulkan CTS run before another GPU experiment. Honor STOP,
   native1000MHz/VID116 and temperature below85C; record health and boot identity.
2. Build against the pinned OpenCL-Headers and clvk import library. On Windows,
   use an isolated application directory with the built OpenCL.dll and its
   dependencies. Verify exports before any system registration. On Linux, link
   the same source against the same clvk revision. Use C11 on both systems.
3. Run `opencl_content_control "<expected GPU name substring>"` with a120second
   external process deadline covering compiler startup and execution. Capture
   stdout/stderr and loaded modules. No unbounded retries after a timeout.
4. Require exit0 and `PASS: 4096 map words, 64 group sums, 0 mismatches`.
   Any OpenCL error, compiler failure, mismatch or deadline stops the series.
   Preserve build logs and inspect GPU state before subsequent work.
5. Compare the full result with Linux on unit A using identical clvk/clspv/Mesa
   revisions. Record version strings and module identities separately.

This control has no performance claim and no image or optional-feature coverage.
The next stages remain supported image kernels and the frozen upstream
OpenCL-CTS subset, with Pass/Fail/Skip and every Windows/Linux difference retained.

## Build sources

PROVENANCE: Khronos OpenCL-Headers Apache-2.0; clvk/clspv Apache-2.0;
LLVM Apache-2.0 with LLVM exception; SPIRV-LLVM-Translator NCSA.
The initial host build uses clvk5515919e12e9e82682bb20eb67e2f0269dd138d3,
clspveaa1c1e92fbddbe809108ba8eebc0ef8908a9af9 and its dependency
LLVM3dc4cbeae3ebb2a6091fe86851f62615970ea17d (24.0.0git).
It enables the online compiler, SPIR-V IL and assertions. This separate compiler
build does not replace the desktop renderer's LLVM23.1.2.
Exact dependency pins, configuration and logs currently live outside the repo
in workspace scratch/m12/opencl-src and scratch/m12/*clvk*.
Archive reproducible source deltas and binary identities with runtime evidence
before promoting a deployment.

## Profiling variant

After the ordinary content run passes, repeat with `--profile`. This creates a
profiling-enabled queue and retains both kernel events. Require nondecreasing
QUEUED/SUBMIT/START/END timestamps and reduction START at or after map END, while
still comparing all output data. Keep the printed raw nanosecond timestamps.
This checks timestamp consistency; it is not the fixed M12 performance benchmark.

clvk enables device queries when calibrated timers are available, or when
CLVK_QUEUE_PROFILING_USE_TIMESTAMP_QUERIES=1 is explicitly set. Record this option
and run the upstream device_timer cases separately to verify the calibrated host
clock. Do not label a host-only fallback as measured GPU execution time.
Runtime results for both variants are pending.

## Reproduce the clvk build

`clvk-source.json` pins every compiler dependency and the five patched clvk files.
Create source directories using its `directory` fields, check out their exact
commits, and apply `clvk-windows.patch` to the clean clvk source. The patch adds
Windows QPC support and removes duplicate identical Ninja job-pool declarations.
The QPC change shares nanosecond conversion with event timestamps and reports
the host clock's resolution. Linux retains its monotonic-clock path.

Run `python build_clvk.py --sources <source-root> --verify-only` first. For the
build, add `--build <build-dir>`. Windows requires an x64 MSVC environment,
`--vulkan-include <Vulkan-Headers/include>` and `--vulkan-library <vulkan-1.lib>`.
Generate a full x64 import library from the official Vulkan-Loader `loader/vulkan-1.def`
if the SDK library is unavailable; the earlier compute-only import library lacks
entry points used by clvk. CMake and Ninja must be on PATH. Keep TEMP/TMP in the
workspace on the development PC. The default is six compiler jobs and one linker.

The script verifies source revisions, patch identity and patched file hashes;
it does not fetch sources, register a driver, or run GPU tests. The Windows build
has completed. A fresh Linux build and paired runtime acceptance remain pending.
