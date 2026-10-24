# D3D12 cube and mip coverage

This standalone client checks cube subresource copies and views. It uses the
system D3D12 runtime and selected adapter. It does not install or change a driver.
It has no window and exits after at most 170 seconds.

The source is unchanged from the reviewed BD-233 diagnostic kit:
`AF56BC0A8EA7903F8CD9E17A033E78C9588EAC07E6E59EA6FA9679D1A78B68A2`.
The original kit executable hash is
`C42D00A332C5E67AFBF4EB3F1851A537B396A4D07E187F53B1578813926033FB`.
New builds have their own identities. The build script records the compiler,
SDK, flags and source/output hashes in `build.json`.

## Build

Use Windows, the Windows SDK, Visual Studio Build Tools libraries, and portable
LLVM 23.1.4 at `toolchain/llvm-23.1.4-win64` below the workspace. The compiler is
explicitly clang-cl, with no MSVC fallback. The executable requires x86-64-v3.
HLSL is embedded in the source and compiled at run time through D3DCompiler.

From the repository root in PowerShell, choose a new output directory:

```powershell
tools/win/d3d12cube/build.ps1 -Workspace P:/BC-250 -Out P:/BC-250/scratch/cube-build-01
```

All compiler temporary files stay in that output directory. A repeat build must
use another output directory. `/Brepro` removes build timestamps. Compare hashes
only with the same compiler, SDK and library versions.

## Run

Run positive and negative controls on each adapter. `nvidia` selects the first
NVIDIA hardware adapter. `default` selects the first hardware adapter in DXGI
performance order. Check the `ADAPTER` line to identify the actual device.
`warp` explicitly selects the software adapter. The debug layer must be installed
unless `-NoDebug` is passed. A run without it is not API validation.

```powershell
$exe = 'P:/BC-250/scratch/cube-build-01/cube-mip.exe'
tools/win/d3d12cube/run.ps1 -Exe $exe -Adapter nvidia -Out P:/BC-250/scratch/cube-nvidia-positive-01
tools/win/d3d12cube/run.ps1 -Exe $exe -Adapter nvidia -Inject -Out P:/BC-250/scratch/cube-nvidia-negative-01
tools/win/d3d12cube/run.ps1 -Exe $exe -Adapter warp -Out P:/BC-250/scratch/cube-warp-positive-01
tools/win/d3d12cube/run.ps1 -Exe $exe -Adapter warp -Inject -Out P:/BC-250/scratch/cube-warp-negative-01
```

The runner creates a hidden child, captures stdout and stderr, enforces the bound,
and records the process exit code and hashes. Each output path must be new.
It accepts these exact results:

| Control | Client exit | Checks | Mismatches | Debug errors |
| --- | --- | --- | --- | --- |
| Positive | 0 | 1215216 | 0 | 0 |
| Negative (`--inject`) | 1 | 1215216 | 2316 | 0 |

The runner succeeds when the expected result and exit code both match. Thus an
accepted negative control has client exit 1. A RESULT line without a captured
exit code does not establish successful process completion.

Direct invocation is also supported:

```powershell
& $exe --adapter default --debug
$clientExit = $LASTEXITCODE
```

## Scope and oracle

The matrix contains two formats, RGBA16_FLOAT and R11G11B10_FLOAT. It uses six or
twelve array slices and all seven mips of a 64 by 64 texture. It covers committed
and placed resources, with copy, UAV or RTV writes. Each slice/mip has a distinct, exactly representable
positive color. Every subresource is first cleared to zero. The negative control
omits the actual writer for slice 4 or 7 at mip 3. It does not change the oracle.

Copy readback checks every texel. Shader reads check five directions per face.
Cube and cube-array SRVs use SampleLevel. A Texture2DArray SRV supplies integer
Load coverage. Views include MostDetailedMip 2, MipLevels 3 and First2DArrayFace 6.
Separate LOD 7 reads check that those three-level views clamp to physical mip 4.
UAV views cover nonzero FirstArraySlice and split slice ranges. RTV views select
one face and mip. Placed resources use a nonzero aligned heap offset. Descriptor
slots remain valid until the command list completes.

Constant colors detect selection of the wrong face or mip but do not detect face
orientation or texel permutations within one subresource. This test does not
cover BC6H, seam filtering, fractional or implicit LOD, sparse residency,
eviction, concurrent queues, or game rendering. A passing result does not close
the BD-233 visual defect.
