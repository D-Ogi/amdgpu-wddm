# shader-edge - shader edge cases that put a fault in the driver, not in a compiler

Two console clients run shader programs at the edge of what compilers and drivers do correctly, read the results
back and compare each word with a value from math. A failure then points at the driver, because the same program
passes on a reference device and the expected value does not come from a compiler.

- `vkshaderedge-x64.exe` (Vulkan, for the hosted RADV ICD) runs HLSL with inline SPIR-V, compiled by DXC
  `-spirv`: atomics with explicit memory semantics, also through a buffer device address, the shader clock, and
  vectors of `half` through a physical pointer at strides 6, 10 and 14 bytes.
- `d3d11shaderedge-x64.exe` (D3D11, for DXVK in our driver) runs one HLSL corpus compiled twice, with FXC and with
  FXC2. If one bytecode fails and the other passes, the fault is between that compiler and the driver. If both
  fail, the fault is in the driver.

The two outside sources: Erfan Ahmadi's inline SPIR-V article and Michael Moroz's FXC2 (links below and in the header
of each source).

Both clients are console programs. They open no window and make no swap chain.

## Files

| File | Content |
|---|---|
| `vk\vkshaderedge.c` | The Vulkan client: command line, device and route witness, the cases and their checks |
| `vk\shaders\inline_spirv.hlsli` | The inline SPIR-V helpers: atomics, pointer conversion, the shader clock |
| `vk\shaders\atomics.hlsl` | The exact-result atomics and the message-passing litmus test, through a buffer or a pointer |
| `vk\shaders\clock.hlsl` | Two clock reads around a known amount of work |
| `vk\shaders\half_bda.hlsl` | `half3`, `half5` and `half7` elements through a physical pointer |
| `d3d11\d3d11shaderedge.cpp` | The D3D11 client: command line, adapter and module witness, the cases and their C oracles |
| `d3d11\shaders\corpus_cs.hlsl` | The compute corpus: float - uint, loops, switches, indexed temps, bit operations |
| `d3d11\shaders\tess_cpid.hlsl` | A tessellation pipeline whose hull shader indexes by `SV_OutputControlPointID` |
| `d3d11\d3d11_corpus.h` | Includes the DXBC headers |
| `gen\*.h` | The compiled programs, one header per program |
| `gen\shaders.txt` | The path, SHA-256 and version of each compiler, and the command line of each header |
| `build.ps1` | The build, the record check and the self-test |

## Build

```
pwsh tools\win\shader-edge\build.ps1 -Kits P:\BC-250\toolchain\nuget
```

The compiler is clang-cl 23.1.4 from `<BC250_ROOT>\toolchain\llvm-23.1.4-win64`, with `-march=x86-64-v3
-mtune=znver2 /O2 /MT /W4 /WX /Brepro`. There is no cl.exe step and no fallback. The SDK headers and libraries come
from the NuGet kits, the MSVC headers and libraries from the installed Visual Studio, the Vulkan headers from
`-VulkanInclude`. The workspace root comes from `BC250_ROOT`, else from `-Kits` (`<BC250_ROOT>\toolchain\nuget`).
The output goes to `<BC250_ROOT>\scratch\build\shader-edge`. A rebuild keeps the previous binary under `retained\` by
its SHA-256. Two builds of the same source give the same bytes.

A normal build reads `gen\` and needs no shader compiler. Two switches use the compilers:

- `-Regenerate` compiles every program again into `gen\` and writes `gen\shaders.txt` again. Commit the result.
- `-Check` compiles every program into `<Out>\gen-check` and compares it with `gen\`, line by line. The compilers
  must be the ones in the record.

Both need `-Dxc`, `-Fxc2` and `-Fxc2No64` (see "FXC2"). `-Dxc` is DXC 1.9.2609.5 from the NuGet package
`Microsoft.Direct3D.DXC`, because the DXC of the Windows SDK has no SPIR-V code generation. FXC is the `fxc.exe`
of the SDK in `-Kits`.

```
pwsh tools\win\shader-edge\build.ps1 -Kits P:\BC-250\toolchain\nuget -Check `
    -Dxc <dxc.exe> -Fxc2 <fxc2.exe> -Fxc2No64 <fxc2 without patch 64>
```

After the link, the build runs `--help` and a bad option on each client. Then it runs a self-test: the Vulkan
client on any device, the D3D11 client on WARP and on any hardware adapter, each with its negative control, and the
FXC2 bytecode without patch 64 on WARP (see "Reference results"). A PC without a Vulkan device skips the Vulkan
part. `-SkipSelfTest` skips all of it.

## Vulkan cases

| Case | What it checks |
|---|---|
| `atomic-exact-ssbo`, `atomic-exact-bda` | 1024 threads. Each thread does a release store and an acquire load of the same word, and an acquire-release add on one counter. Then it does a compare-exchange with acquire-release and acquire semantics. Every word has one correct value. |
| `atomic-mp-ssbo`, `atomic-mp-bda` | Message passing: each of 4096 threads writes 4 payload words and sets a flag with a release store. Its partner in the other half of the dispatch spins on the flag with acquire loads (at most 65536). Then it reads the payload. A seen flag with an old payload is a failure. |
| `clock-subgroup`, `clock-device` | `OpReadClockKHR` at subgroup and device scope: the second read is not lower than the first and not zero. At device scope, two submissions run in order. The lowest first read of the second submission must not be lower than the highest last read of the first. |
| `half3-scalar`, `half5-scalar`, `half7-scalar` | Elements of 3, 5 and 7 `half` (6, 10 and 14 bytes) through a physical pointer, read and written one component at a time. The guard bytes around the output stay intact. |
| `half3-vector`, `half5-vector`, `half7-vector` | The same elements as `half3`, `half4` + `half` and `half4` + `half3` vectors. This is the access of BD-248. |

The `-ssbo` cases use a storage buffer, the `-bda` cases convert a 64-bit address to a pointer with
`OpConvertUToPtr`. Each case runs with the work buffer in host memory and in device-local memory
(`--placement both`, the default).

The input values of the `half` cases have at most 11 significant bits, so that binary16 holds each one and each
result exactly. A case whose feature is missing (`VK_KHR_shader_clock`, `storageBuffer16BitAccess`,
`shaderFloat16`) is a SKIP with the name of the feature. `bufferDeviceAddress` and `shaderInt64` are required.

The inline SPIR-V comes from DXC attributes `vk::ext_instruction`, `vk::ext_reference`, `vk::ext_literal`,
`vk::ext_capability` and `vk::ext_extension`. DXC ignores `vk::ext_capability` on an entry point, so the `half`
programs do not declare `StorageBuffer16BitAccess`. DXC validation accepts them without it.

## D3D11 cases

Each case runs the FXC bytecode and the FXC2 bytecode of the same source (`--compiler` selects one).

| Case | What it checks | Instructions, FXC / FXC2 |
|---|---|---|
| `fu` | float - uint, uint - float, `float2 - uint2` and float - int, with unsigned values on both sides of 2^31 (FXC2 patch 47) | 33 / 59 |
| `loop` | Loops with `continue` and `break`, a trip count from data, nested loops (patch 1) | 78 / 80 |
| `switch` | Switches with shared labels, a branch and a loop in a case, a default, a switch that returns. FXC keeps 3 `switch` instructions, FXC2 makes 11 `if` instructions and no `switch` (patches 20, 26, 29). | 101 / 104 |
| `indexed` | Local arrays with run-time indices (indexable temps) | 54 / 84 |
| `bits` | `firstbithigh`, `firstbitlow`, `countbits`, `reversebits`, shifts and masks (FXC: `ubfe`), unsigned division by constants (patches 6, 53, 15) | 37 / 70 |
| `tess-array` | A hull shader that copies the control points into local arrays and indexes them by `SV_OutputControlPointID` (patch 64) | |
| `tess-direct` | The same hull shader, indexing the input patch directly | |

The compute cases run 256 threads, and each thread writes 8 words. The client compares all 2048 words with a C copy
of each function. The tessellation cases draw one patch of three control points over a 64 by 64 target. Each pixel
must hold the value `51020003`, which the domain shader makes from the three control points.

`--with-no64` adds the two tessellation cases compiled by FXC2 without its patch 64. For `tess-array`, FXC2 without
that patch makes `mov o0.xyzw, x0[vOutputControlPointID0.x + 0].xyzw`: an indexable temp indexed by the output
control point ID. FXC indexes the input patch (`vicp[r0.x]`), and FXC2 with patch 64 does too. On an RTX 5070
Laptop, that bytecode removed the device. It is a robustness probe, not a correctness reference. It can remove
the device, so it runs only on request, after the other cases.

## Options and exit codes

```
vkshaderedge-x64.exe [--icd DLL | --manifest JSON] [--device N] [--any-device] [--list-devices]
                     [--placement host|local|both] [--case NAME] [--negative-control]
                     [--deadline SEC] [--fence-timeout-ms N]
d3d11shaderedge-x64.exe [--adapter N | --warp] [--any-adapter] [--list-adapters] [--case NAME]
                        [--compiler fxc|fxc2|fxc2no64] [--with-no64] [--debug] [--negative-control]
                        [--deadline SEC]
```

- Both clients use the BC-250 by default and refuse another device unless `--any-device` or `--any-adapter` is
  given.
- `--icd` loads the ICD DLL directly, without the Vulkan loader. The client prints the path and SHA-256 of the ICD
  module that it loaded, and the D3D11 client prints the driver modules in its process, so the log shows which
  driver ran.
- `--case NAME` runs the cases whose name starts with NAME.
- `--negative-control` makes every program compute a known wrong result. The D3D11 programs and the Vulkan
  atomics add 1. The message-passing readers read the payload of a different thread. The clock programs swap
  their two reads. The `half` programs multiply the element index by N, as in BD-248. Every case must then fail.
  If a case passes, the check cannot see the error, and the client prints BROKEN and exits with 3.
- `--deadline` stops the run after SEC seconds (default 60, at most 170) from a watchdog thread.

| Exit | Meaning |
|---|---|
| 0 | Every case passed or was a SKIP |
| 1 | A case failed (with `--negative-control`: every case failed, as required) |
| 2 | Usage |
| 3 | API or system error, a removed device, or a negative control that passed |
| 4 | Deadline |

## Reference results

RTX 4090 of the development PC, NVIDIA driver 0x94D84000 (`nvoglv64.dll` and `nvwgf2umx.dll` from the same
DriverStore package), 2026-10-28:

| Client | Run | Result |
|---|---|---|
| Vulkan | `--any-device` | 24 PASS of 24 (12 cases, host and local memory), 0.3 s. Message passing: 4096 of 4096 threads saw the flag. |
| Vulkan | `--any-device --negative-control` | 24 FAIL of 24, exit 1 |
| D3D11 | `--any-adapter` | 14 PASS of 14, 0.4 s |
| D3D11 | `--any-adapter --negative-control` | 14 FAIL of 14, exit 1 |
| D3D11 | `--warp` | 14 PASS of 14 |
| D3D11 | `--warp --negative-control` | 14 FAIL of 14, exit 1 |
| D3D11 | `--warp --with-no64 --case tess-direct` | 3 PASS of 3 |
| D3D11 | `--warp --with-no64 --case tess-array` | `fxc2no64` FAIL: WARP writes no pixel, and the debug layer (`--debug`) reports no message |

The FXC2 author reports that WARP draws the bytecode without patch 64 correctly. The WARP of Windows 11 26200 on the
development PC draws nothing with it, and it reports no error. The hostile bytecode was not run on the RTX 4090.

## FXC2

FXC2 (https://github.com/MichaelMoroz/FXC2) is an `fxc.exe`-compatible front end over the HLSL compiler of Wine's
vkd3d-shader, with 65 patches. Here it is a tool only, used to make the headers in `gen\`. It is not in this
repository, and no package ships it. The record in `gen\shaders.txt` holds the version and SHA-256 of each tool
build.

The tool builds of the record were made natively on Windows with clang-cl 23.1.4, not with the mingw script of
the FXC2 repository:

1. Clone FXC2 at `f8ad188f`, and Wine vkd3d at the FXC2 base `cfcb4833` (`vkd3d-2.1-93`). Apply the 65 patches of
   `FXC2\patches` in order.
2. Make the generated files:
   - `widl` (mingw-w64-tools 14) over the IDL files of `include\`, without `vkd3d_unknown.idl`
   - `make_spirv` (Perl, with a `JSON` module that maps to `JSON::PP`) for `spirv_grammar.h`
   - `win_bison` and `win_flex --wincompat` for the HLSL and preprocessor parsers
   - one-line `vkd3d_version.h` and `fxc2_version.h`
3. Compile `libs\vkd3d-common`, `libs\vkd3d-shader`, `libs\vkd3d-utils` and `FXC2\src\fxc2.c` with
   `/O2 /MT /std:c17 -march=x86-64-v3 -mtune=znver2`, `-D_USE_MATH_DEFINES -DVKD3D_NO_TRACE_MESSAGES`, a
   `config.h` that sets the `HAVE_BUILTIN_*` macros, and the `D3DCompile*` entry points of vkd3d-utils renamed to
   `vkd3d_D3DCompile*`. Stubs that return `E_NOTIMPL` replace the vkd3d device and root signature functions that
   vkd3d-utils calls. Link with `lld-link`.
4. For `-Fxc2No64`, reverse `0064-vkd3d-shader-ir-A-relative-index-from-a-temp.patch` and build again.

The positive control of that build: the native FXC2 and the prebuilt `bin\fxc2.exe` of the FXC2 repository give the
same bytes for 25 of the test shaders of the FXC2 repository. A 26th fails to compile in both.

## Lab use

On the lab, the Vulkan client loads the installed RADV ICD directly:

```
vkshaderedge-x64.exe --icd "C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_radv.dll" --deadline 120
d3d11shaderedge-x64.exe --deadline 120
```

Each run takes less than a second on the reference device. A lab trial runs the cases, then the negative control.
The `--with-no64` run is the last step of a trial and runs only on request, because it can remove the device.

## Credits

The inline SPIR-V cases follow Erfan Ahmadi, "Inline SPIR-V" (https://erfan-ahmadi.github.io/blog/inline-spirv/), and
the D3D11 corpus follows the edge cases of Michael Moroz's FXC2 (https://github.com/MichaelMoroz/FXC2).
