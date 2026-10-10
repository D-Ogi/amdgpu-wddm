# CPU bandwidth of mapped graphics memory

`cpumembw` measures CPU traffic through Vulkan host-visible memory types and native D3D12 UPLOAD/READBACK heaps.
It does not submit GPU work. A CPU-only arm supplies the positive control. Each invocation starts a new child
process, so the selected RADV heap policy is set before the loader, instance or D3D12 device is created.

The parent does not load a graphics runtime. It owns a Job Object with kill-on-close and an elapsed-time watchdog.
The default total budget is 150 seconds, including a five-second child termination reserve. Exit 124 means timeout,
never a successful sample. The `child_exited` field says whether termination was observed. A hung kernel call can
prevent process termination. A user-mode watchdog cannot promise recovery of a stalled kernel or GPU. The lab
operator must retain the normal independent thermal/STOP supervisor and recovery channel.

## Build and host verification

Use an existing MSVC installation, SDK NuGet packages and Vulkan headers. No installer or Vulkan import library is
needed. Build output and test output go to the caller's output directory.

```powershell
./build.ps1 -Kits P:/BC-250/toolchain/nuget `
  -VulkanInclude P:/BC-250/ref/Vulkan-Headers/include `
  -Out P:/BC-250/scratch/seg0-cpu-write/build
python -B ./test_host.py P:/BC-250/scratch/seg0-cpu-write/build/cpumembw.exe `
  --output P:/BC-250/scratch/seg0-cpu-write/build/host-tests.json
```

The build uses `/W4 /WX /O2 /MT` and runs only `--self-test`. The host suite runs the CPU path, malformed-input
controls, a deliberate corruption in the actual measurement validation path, and a sleeping child killed by the
watchdog. It does not load Vulkan or D3D12. The non-page-aligned 65544-byte case exercises the final partial chunk.

## Lab commands

Use the adapter index and LUID of the intended adapter. Vulkan enumeration indices and DXGI indices need not agree.
`--luid` is 16 hexadecimal digits: high 32 bits followed by low 32 bits, without `0x`. It is checked before allocation.
The device event prints the selected name, IDs and LUID. A missing or mismatched LUID is an error when requested.
Do not infer adapter identity from index zero alone.

```text
cpumembw.exe --api cpu --bytes 67108864 --duration .25 --timeout-ms 150000
cpumembw.exe --api vulkan --adapter N --luid HHHHHHHHLLLLLLLL --heap default --type all --bytes 67108864 --duration .25 --timeout-ms 150000
cpumembw.exe --api vulkan --adapter N --luid HHHHHHHHLLLLLLLL --heap unified --type all --bytes 67108864 --duration .25 --timeout-ms 150000
cpumembw.exe --api d3d12 --adapter N --luid HHHHHHHHLLLLLLLL --type all --bytes 67108864 --duration .25 --timeout-ms 150000
```

Each line is a separate arm. Stage the executable and preserve both stdout JSONL and stderr with its SHA-256,
stack identities and the operator's thermal/STOP logs. No GPU arm has been run on the development PC.
`--heap default` explicitly sets `radv_enable_unified_heap_on_apu=false`. Unified sets `true`. It does not modify
persistent settings. Other inherited driver options remain inherited and must be captured by the lab manifest.

`--type N` selects a Vulkan memory-type index. For D3D12, 0 means UPLOAD and 1 means READBACK. The two D3D12 heaps
use the system `d3d12.dll` and `dxgi.dll`, loaded with `LOAD_LIBRARY_SEARCH_SYSTEM32`. There are no Agility exports.
Vulkan uses the system `vulkan-1.dll` loader and the configured ICD discovery environment. A Vulkan allocation must
be compatible with the transfer buffer's `memoryTypeBits`. Non-host-visible and incompatible types are not measured.
AMD device-coherent types are explicitly skipped because this tool does not enable the optional
`deviceCoherentMemory` feature. A selection with no measurable type fails instead of reporting success.

`--bytes` is 4096 to 268435456, divisible by eight. `--duration` is .01 to 2 seconds per phase and type. Default .25.
`--chunk-bytes` is 4096 to 1048576, divisible by eight. Default 262144. `--timeout-ms` is 100 to 150000. A large type
set may exhaust the total budget. Preserve the partial output and select fewer types in the next arm. The internal
`--child` switch is for the supervisor only. Do not invoke it as a lab command.

## What each number means

The kernels run in this order on each allocation:

1. Sequential volatile 64-bit stores of an offset-dependent pattern.
2. `memcpy` from an initialized normal-RAM source into mapped memory.
3. Volatile 64-bit loads with a checksum into a live sink.
4. `memcpy` from mapped memory into normal RAM, followed by content validation.

A 4096-byte read pilot chooses a read sweep of approximately 20 ms, capped at the requested allocation size and
floored at 4096 bytes. This prevents an entire large UC/WC allocation from being read just to discover it is slow.
The pilot, source initialization, destination allocation, content checks and API cache operations are outside the
bandwidth timings. The pilot is not a timeout for driver calls. The process watchdog remains the bound.

The write phases use the requested allocation span. They process up to `chunk_bytes` per clock/deadline check. A
short phase can end after only a prefix. The emitted `bytes`, `span_bytes`, validation `initialized_bytes` and
`checked_bytes` make this explicit. Each phase reports bytes actually processed, not bytes requested. The read
phases use the adaptive prefix. Validation reads that prefix, not every byte of a possibly slow large allocation.
The complete read prefix is initialized outside timing before read measurements. Its timed volatile-read checksum
is checked, and the copied RAM destination is checked after the readback phase. Deliberate corruption makes the
process fail. Zero-filled pages are not accepted as the expected data.

Write timing includes `_mm_sfence()` after the last store to drain write-combining buffers. Vulkan noncoherent
memory is flushed after writes and invalidated before reads, outside the measured interval. The mapped range
starts at zero and covers `VK_WHOLE_SIZE`, including the allocation end, so nonCoherentAtomSize alignment is honored.
Host-coherent allocations omit those API calls. There are no concurrent GPU accesses and no GPU ownership claim.
D3D12 maps the complete CPU-readable buffer range and unmaps with the written range. The diagnostic intentionally
reads UPLOAD memory and writes a pattern into READBACK memory. It measures the CPU mapping rather than a production
GPU upload/readback sequence. It does not claim GPU visibility, GPU copy throughput or a fence-checked GPU result.

`api_latency` separates allocation/creation and map time from CPU access. `VirtualQuery` logs page-protection flags
and region size without printing the mapping address. These flags are observations from the Windows API, not a
proof of effective PAT/MTRR attributes. D3D12's `GetCustomHeapProperties` is also a driver/runtime report, not a CPU
cache-register measurement. Vulkan HOST_CACHED and HOST_COHERENT are distinct promises.

Rates are decimal GB/s. Repeated sweeps can benefit from CPU caches. The adaptive read prefix can fit entirely in
cache. Always compare the same span, kernel, chunk size and timing. The sequential store kernel also computes a per-word pattern. The volatile scalar load kernel and a library
`memcpy` can have very different throughput on the same mapping. Do not attribute their difference to the driver.
The CPU positive control measures loop/timer overhead with the same chunk policy. Repeat with larger chunks when
that overhead matters. A 4 KiB chunk is offered as a deadline-sensitive diagnostic, not the default bandwidth claim.

## Contracts

Local-first sources used during implementation:

- Vulkan-Docs revision `01aaacd99480487bf63830959513c5ca8ceb996d`, `chapters/memory.adoc`, memory-property and mapped-memory
  sections. HOST_COHERENT removes the need for host flush/invalidate operations. HOST_CACHED means host caching.
  Uncached host reads can be much slower. Public: <https://docs.vulkan.org/spec/latest/chapters/memory.html>.
- Vulkan-Headers revision `b0c3dd6851e22621f194306d511b9253e4c1577f`, included at build time. No header or library is
  copied into the repository. PROVENANCE: Khronos Vulkan-Headers, Apache-2.0 OR MIT.
- SDK API documentation revision `a4fd3f7efe2e3378a96c6fe5a6a9455eba9fa021`,
  `content/d3d12/nf-d3d12-id3d12resource-map.md`. CPU reads from UPLOAD work but can be prohibitively slow. Mapped
  WRITE_COMBINE pointers have weaker ordering than normal WRITE_BACK pointers. Public:
  <https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map>.
- SDK 10.0.26100.0 `um/d3d12.h`, heap types and CPU page properties. Resources on UPLOAD use GENERIC_READ.
  READBACK uses COPY_DEST. This tool creates buffers, not textures, on these heaps.

This instrument measures CPU traffic. The existing `vkmembw` shader dispatch results measure GPU traffic and are
not interchangeable with these results. Neither is sufficient by itself to identify the cause of an LLM prefill
regression. Preserve allocation flags, per-type selection, configuration and both read kernels for that diagnosis.

## Isolated candidate ICD

For an elevated lab process, `VK_DRIVER_FILES` can be ignored by the Vulkan loader. Use the optional direct route
for an exact candidate without registry edits or deployment:

```text
cpumembw.exe --api vulkan --icd P:/candidate/vulkan_radeon.dll --adapter N --luid HHHHHHHHLLLLLLLL --heap unified --type all --timeout-ms 150000
```

`--icd` is Vulkan-only. It accepts an absolute local drive path to a DLL, with ASCII characters and spaces.
Relative paths, UNC/device paths, alternate data streams, dot components and ambiguous trailing dots/spaces are
refused before a child starts. The default system-loader route is unchanged. The direct route bypasses loader
layers and manifests, loads dependencies only from the DLL directory and System32, and follows the existing
`vkfillcheck` direct ICD route: get `vk_icdGetInstanceProcAddr` (or its legacy fallback), negotiate the ICD interface
when available, then create the Vulkan instance. Failed negotiation is an error. An absent negotiation export is
reported as legacy interface zero.

The JSONL names `system-loader` or `direct-icd` explicitly and prints the requested and loaded DLL paths. Hash the
exact candidate separately in the operator manifest before and after the arm. The tool reports that this hash is
required. It does not compute a file hash and does not claim a path alone identifies immutable bytes. Preserve the
candidate's dependent DLLs and their hashes too. Direct-route results are not measurements of normal loader discovery.
The host suite checks invalid paths and child command-line quoting with a sleeping child before any loader call.
No real ICD, Vulkan loader or D3D12 runtime is loaded by these tests.
