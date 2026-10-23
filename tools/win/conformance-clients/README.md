# conformance-clients - ROV, conservative rasterization, indirect DispatchRays and mesh shaders

`amdgpu_wddm_conformance.exe` is a native D3D12 client for two M15 criteria. M15.3 asks for a functional ROV test
and a conservative rasterization test with image oracles. M15.5 asks for indirect `DispatchRays`. The criteria are
in `docs/m15-reconciliation.md`. `amdgpu_wddm_mesh.exe` is the mesh shader witness of the same kind, for the D3D12
mesh shader path of the UMD (`driver/umd/d3d12/engine-ddi`).

Both clients are x64 and offscreen. They open no window and they create no swap chain. They load `d3d12.dll` from
System32 and carry no application-local runtime DLL. Each subtest has an exact CPU oracle. Each subtest also has
negative controls, which show that the comparison can fail.

## Files

| File | Content |
|---|---|
| `main.cpp` | The command line, the standalone run and `--selftest` |
| `conformance.h` | The three subtests, their oracles and their comparators |
| `shaders\raster.hlsl` | Vertex program and the pixel programs of the ROV and conservative passes |
| `shaders\dxr.hlsl` | Ray pipeline: two ray generation shaders, a miss shader and a triangle hit group |
| `shaders\dxr_args.hlsl` | Compute program that writes the indirect arguments on the GPU |
| `mesh.cpp` | The mesh shader witness: its command line, its oracle, its comparator and `--selftest` |
| `shaders\mesh.hlsl` | Vertex, pixel, mesh and amplification programs of the witness, all shader model 6.5 |
| `gen\*.h` | The dxc output of those shaders, one header per program |
| `gen\dxc.txt`, `gen\dxc-mesh.txt` | The dxc path, version, hash and command lines behind those headers |
| `build.ps1`, `build-mesh.ps1` | The builds and the CPU-only gates after each link |
| `check-shaders.ps1` | The `conformance-shaders` gate: every header reproduces from its record |
| `interactive-check.ps1` | Host check of the lab protocol through the `d3d12queue` controller |

`conformance.h` is the conformance variant of the copy verb of `tools\win\d3d12queue`.
`d3d12queue\interactive.h` includes it when the build defines `INTERACTIVE_CONFORMANCE`, and routes the copy verb
to `conformance_run()`. It is the only variant outside that directory, so the build puts both directories on the
include path.

## Build

```
pwsh tools\win\conformance-clients\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\amdgpu_wddm_conformance
```

MSVC comes from vswhere. The SDK 10.0.26100 headers and libraries come from the NuGet kits. The flags are
`/W4 /WX /O2 /MT /EHsc /std:c++17` with `INTERACTIVE_CONFORMANCE` and `INTERACTIVE_FEATURE_LEVEL_12_1`. Temporary
files stay in the workspace, never on the system drive.

`-Out` is optional. Its default is `<BC250_ROOT>\scratch\build\amdgpu_wddm_conformance`. The workspace root comes
from `BC250_ROOT`, and else from `-Kits`, which points at `<BC250_ROOT>\toolchain\nuget`. The script never guesses
the root from its own place in the file system, because a build from a git worktree sits somewhere else.

The script first compiles the shaders with the SDK's dxc 1.8.2502.11 into `gen\*.h`. That compiler is
deterministic, so a rebuild with the same SDK writes the same bytes as the tracked headers. The script
normalizes the line endings of each header after the compiler, because dxc writes CRLF and `.gitattributes`
checks the headers out with LF. A build therefore leaves no tracked file reported as modified. It writes the record
`gen\dxc.txt` only when the record changed, and only when the dxc path starts with the workspace root. A build with
a dxc outside that root keeps the tracked record and prints a warning, so no absolute toolchain path of one machine
reaches the repository. The gate `conformance-shaders` of `tools\quality\quick.ps1` checks the same thing without a
build. Three gates run after the link: `--help` exits 0, `--invalid` exits 2 and `--selftest` exits 0. The previous
exe stays under `retained\` by its hash.

`-TestRefuseSignature` builds a host arm that treats the `DISPATCH_RAYS` command signature as refused, as the
D3D12 shell refuses it today. The arm gets its own output directory. It checks the client's report of that path,
and it is never a lab artifact.

## Run

```
amdgpu_wddm_conformance --interactive DIR --deadline S        lab protocol (BC-250 by VEN_1002 DEV_13FE)
amdgpu_wddm_conformance --interactive-warp DIR --deadline S   the same protocol on WARP
amdgpu_wddm_conformance --adapter warp|bc250|hw|index:N --out DIR [--deadline S] [--debug-layer] [--only rov,cr,dxr]
amdgpu_wddm_conformance --selftest
```

Interactive mode keeps the protocol of `tools\win\d3d12queue`: `command-NNNNNN.txt`, `result-NNNNNN.json`,
`trace.jsonl` and `session.json`. The copy verb runs the three subtests. It writes `DIR\conformance.json` and the
dumps. It returns S_OK only when all three subtests pass, which also sets `copy_success` true. It returns
DXGI_ERROR_UNSUPPORTED when nothing failed and something was skipped. In every other case it returns the first
failure's HRESULT. The process exit code stays the protocol's, so the verdict is in the receipt and in the
`CONFORMANCE` lines.

Standalone mode creates the device at FL 12_1 on the named adapter. `hw` takes the first hardware adapter that is
not Microsoft's and that creates an FL 12_1 device. Exit codes: 0 all pass, 1 a failure or an error, 4 skip only,
2 bad usage, 3 no device or GPU work that did not retire. `--debug-layer` is for the host. It turns on the SDK
layers and it fails the run on any corruption or error message.

The host check of the lab protocol drives the client through five commands with the `d3d12queue` controller:

```
pwsh tools\win\conformance-clients\interactive-check.ps1 -Directory P:\BC-250\scratch\conformance\interactive-warp
```

## Output

One line per subtest, then the JSON file:

```
FEATURES ROVsSupported=.. ConservativeRasterizationTier=.. RaytracingTier=.. MaxSupportedFeatureLevel=.. HighestShaderModel=.. device=12_1
CONFORMANCE <rov|conservative|dxr-indirect> <PASS|FAIL|SKIP|ERROR> mismatches=N checked=N first=<pass>:x=..,y=..,got=0x..,want=0x.. <details> [reason=..] hr=........
CONFORMANCE overall <PASS|FAIL|SKIP> hr=........
```

A missing tier gives SKIP with a `reason=`: `ROVsSupported=0`, `ConservativeRasterizationTier=0`, or a ray tracing
tier below 1.1. `conformance.json` holds the features, the adapter, the driver modules loaded in the process, and
per subtest the counts, the first mismatch, the controls and the `command_signature_hr`.

The dumps hold the raw read-back words, little-endian UINT32. `rov.bin` holds four 64x64 images: the ROV texture,
the plain UAV texture, and the render target of each pass. `conservative.bin` holds four 64x64 images: standard,
conservative, inner coverage, and inner coverage of the back-facing copies. `dxr.bin` holds 1024 words, with five
sections of 128 words, the GPU-written arguments at word 640 and the counts at word 704.

## Subtests

**rov.** 43 triangles in one `DrawInstanced`: three full-screen triangles between 20 rectangles of two triangles
each, 23 layers, every pixel covered 3 to 9 times. The pixel shader folds `v = v * 0x01000193 + (tag ^ pixel)`
into a `RasterizerOrderedTexture2D<uint>` with a per-run random start value. The oracle is the same fold over the
covering primitives in API order. No pixel centre lies on an edge of a rectangle, which the client checks at run
time, so coverage never depends on the top-left rule. Controls: the reversed-order oracle differs at all 4096
pixels, and the render target checks the coverage the oracle assumed. The same program through a plain
`RWTexture2D` is recorded as an observation only.

**conservative.** Eight triangles in separate 16x16 cells: a horizontal and a vertical sliver without pixel
centres, a diagonal sliver, a sub-pixel triangle inside one pixel, one across a pixel corner, one across a column
boundary, a right triangle and a general one. The vertices lie on a 1/16 grid, which is exact under 16.8 snapping.
The passes are conservative raster off, conservative raster on, and at tier 3 conservative raster on with
`SV_InnerCoverage`. The rules come from DirectX-Specs `d3d\ConservativeRasterization.md` at 5a4139be, with a
1/64-pixel margin. A pixel must be covered when the triangle meets its square shrunk by 1/64. It must not be
covered when the triangle misses the square grown by the tier's uncertainty plus 1/64. The uncertainty is 1/2 at
tier 1 and 1/256 at tiers 2 and 3. The inner bit must be set when the square grown by 1/256 plus 1/64 lies inside
the triangle, and it must be clear when the square shrunk by 1/64 does not. At tiers 2 and 3 the geometry leaves
no pixel between "must" and "must not", which the client checks at run time. At tier 1 the 142-pixel band accepts
either value and the client counts it. The expected counts are 137 standard, 235 conservative and 82 inner pixels.
Controls: the standard and conservative oracles differ in 98 pixels, and each image is read against the other
pass's oracle.

**dxr-indirect.** This subtest needs ray tracing tier 1.1. The scene is one triangle in a bottom and a top level
acceleration structure. The pipeline is a `lib_6_3` library with two ray generation shaders, a miss shader and a
triangle hit group. Every output word names the hit (1) or the miss (2), the ray generation record (0x0A or 0x0B)
and the dispatch width and height. A compute shader writes both `D3D12_DISPATCH_RAYS_DESC` records of 104 bytes
and the count words 1 and 2 into DEFAULT buffers that the CPU never touches. One output buffer holds five
sections: direct `DispatchRays` as the positive control, `ExecuteIndirect` with max 1 and no count buffer, max 2
with count 1, max 2 with count 2, and max 2 with no count buffer. Words that no dispatch writes keep the prefill.
Controls: the count-1 and the count-2 oracles differ in 32 words, and the GPU-written arguments and counts are
read back and compared. A refused `DISPATCH_RAYS` command signature is a FAIL with that HRESULT, and the direct
section still runs.

Every read-back region starts at the complement of its expectation. The client reads it again before the
submission, so an untouched word can never compare equal. Each submission waits at most 10 s. The objects of a
submission whose retirement is not proven are never released. `--selftest` checks the oracles, the design
invariants and every comparator on ideal, reversed, swapped, perturbed, skipped, truncated and count-ignored
images, in 36 checks.

## Lab evidence

Trials 237-240 of the native D3D12 series ran this client on unit A. The records are in
`evidence/windows/2026-10-02-E50-native-d3d12-system-runtime` (facts M781 and M782), whose write-up predates this
directory and says that the client source was not in the repository yet. The exe of those trials carries the
SHA256 prefix 2BB5CC0D. ROV passed in all four trials. Conservative rasterization passed at tier 3 from trial 239,
on an ICD with the RADV inner-coverage change. Indirect `DispatchRays` passed in trial 240 with all five sections
exact.

The registered D3D12 shell reports ray tracing tier 0 by default, so `dxr-indirect` only skips. A lab trial that
wants the subtest sets the shell's `raytracing-tier` experiment, as section 5 of that write-up states.

## Host controls (2026-10-01)

Runtime 10.0.26100.9278. The inbox WARP reports everything the subtests need, so no Microsoft.Direct3D.WARP
package was taken.

WARP, `--adapter warp --debug-layer`, exit 0, last trace event at 125 ms:

```
runtime=system32/d3d12.dll adapter=VEN_1414 DEV_008C Microsoft Basic Render Driver (software)
FEATURES ROVsSupported=1 ConservativeRasterizationTier=3 RaytracingTier=1.1 MaxSupportedFeatureLevel=12_1 HighestShaderModel=6_8 device=12_1
CONFORMANCE rov PASS mismatches=0 checked=12288 first=none rov=0/4096 rt=0/8192 layers=3..9 reversed_oracle_mismatch=4096/4096 plain_uav_observed=0/4096 comparator_selftest=ok hr=00000000
CONFORMANCE conservative PASS mismatches=0 checked=12288 first=none tier=3 std=0/4096 cons=0/4096 inner=0/4096 expected_std_pixels=137 expected_cons_pixels=235 expected_inner_pixels=82 uncertainty_band=0 uncertainty_band_covered=0 control_std_image_vs_cons_oracle=98 control_cons_image_vs_std_oracle=98 oracle_differ=98 backface_inner_observed=153/4096 hr=00000000
CONFORMANCE dxr-indirect PASS mismatches=0 checked=694 first=none variants=direct:0/128,indirect-max1:0/128,count1-of-max2:0/128,count2-of-max2:0/128,max2-no-count:0/128 gpu_args=0/54 hits=12 misses=52 control_count1_vs_count2_oracle=32/128 hr=00000000
CONFORMANCE overall PASS hr=00000000
DEBUGLAYER errors=0 warnings=7
```

The seven warnings are 929 and 1328. 929 covers three CPU writes to READBACK memory, which is the complement
prefill, as in the `d3d12queue` variants. 1328 covers four buffers created in a state other than COMMON, which
buffers ignore.

RTX 4090, driver 32.0.15.9597, `--adapter hw --debug-layer`, exit 0, last trace event at 562 ms:

```
runtime=system32/d3d12.dll adapter=VEN_10DE DEV_2684 NVIDIA GeForce RTX 4090
FEATURES ROVsSupported=1 ConservativeRasterizationTier=3 RaytracingTier=1.2 MaxSupportedFeatureLevel=12_2 HighestShaderModel=6_8 device=12_1
CONFORMANCE rov PASS mismatches=0 checked=12288 first=none rov=0/4096 rt=0/8192 layers=3..9 reversed_oracle_mismatch=4096/4096 plain_uav_observed=3868/4096 comparator_selftest=ok hr=00000000
CONFORMANCE conservative PASS mismatches=0 checked=12288 first=none tier=3 std=0/4096 cons=0/4096 inner=0/4096 expected_std_pixels=137 expected_cons_pixels=235 expected_inner_pixels=82 uncertainty_band=0 uncertainty_band_covered=0 control_std_image_vs_cons_oracle=98 control_cons_image_vs_std_oracle=98 oracle_differ=98 backface_inner_observed=0/4096 hr=00000000
CONFORMANCE dxr-indirect PASS mismatches=0 checked=694 first=none variants=direct:0/128,indirect-max1:0/128,count1-of-max2:0/128,count2-of-max2:0/128,max2-no-count:0/128 gpu_args=0/54 hits=12 misses=52 control_count1_vs_count2_oracle=32/128 hr=00000000
CONFORMANCE overall PASS hr=00000000
DEBUGLAYER errors=0 warnings=7
```

The plain-UAV observation is the practical negative control of the ROV test. On this hardware the same fold
without ROV misses the ordered result in about 95 % of the pixels. On WARP it misses in none, because WARP runs
the pixels in order.

`--selftest` prints `SELFTEST PASS failures=0`.

## The mesh shader witness

`amdgpu_wddm_mesh.exe` draws the same eight triangles three times and compares the read-backs word for word:

| Pass | Pipeline | Draw |
|---|---|---|
| `reference` | a vertex program and a pixel program | `DrawInstanced(24, 1, 0, 0)` |
| `mesh-ms` | a mesh program and the same pixel program | `DispatchMesh(8, 1, 1)` |
| `mesh-as` | an amplification program, the mesh program and the same pixel program | `DispatchMesh(1, 1, 1)` |

Each triangle sits in its own 16x16 cell of a 64x64 `R8G8B8A8_UNORM` target, so a wrong triangle is a wrong cell.
The pixel program writes a vertex tag and a primitive tag, both `nointerpolation`, so a pixel takes them from the
first vertex of its triangle. The vertex program carries both tags per vertex. The mesh program carries the vertex
tag on its vertex output and the primitive tag on its primitive output. A mesh pipeline whose primitive output does
not reach the pixel input therefore writes the wrong primitive tag and fails on every covered pixel, instead of
passing with a plausible picture. The amplification program dispatches the eight mesh groups and gives each one its
triangle in the payload, so a payload that does not arrive draws the wrong cells.

The vertex program reads `SV_VertexID` and the pipelines have no input layout and no vertex buffer, which a mesh
pipeline must not have either. The root signature holds no parameters. The mesh pipelines are created through the
subobject stream of `ID3D12Device2::CreatePipelineState`, which is the only path on which the engine reads the AS
and MS subobjects.

Before any comparison the reference itself is checked: every word of it is the clear value or one of the eight
expected words, every expected word lies inside its own triangle's 16x16 cell, and every triangle covers at least
50 pixels (the geometry covers 70 pixel centres). A reference that fails that oracle is an ERROR, not a pass. The
positional rule is what makes the comparison a statement about the geometry: `corner()` insets each triangle by two
pixels, so no expected word can legitimately fall outside its cell, and without that rule a reference that drew the
eight words in the wrong cells passes the oracle and a mesh image wrong in the same way compares byte-equal to it
and reports PASS. The run-time negative control compares each mesh image with the
reference whose primitive tags are moved to the next triangle. That count must equal the covered pixel count, so
a blank-against-blank comparison cannot pass. Each read-back region starts at `0x5A5A5A5A`, which no pass can write,
so a pass that never ran cannot compare equal.

```
pwsh tools\win\conformance-clients\build-mesh.ps1 -Kits P:\BC-250\toolchain\nuget
amdgpu_wddm_mesh --adapter warp|bc250|hw|index:N [--out DIR] [--deadline S] [--debug-layer] [--only ms,as]
                 [--isolate 0|1]
amdgpu_wddm_mesh --selftest
```

The build, the flags and the shader record follow `build.ps1` above, with the record `gen\dxc-mesh.txt`. The three
gates after the link are the same: `--help` exits 0, `--invalid` exits 2 and `--selftest` exits 0. `--selftest`
checks the oracle and the comparator on ideal, unknown-word, exchanged-cell, missing-triangle, thin-triangle,
flipped, permuted-tag and never-run images, in ten checks. `-TestForceTier0` is a host arm that reads the tier as 0 on
every adapter, so the skip path can be checked where no tier-0 adapter is at hand. It is never a lab artifact.

`--isolate 1` (the default) records every pass in its own command list and submits it with its own fence and
its own bounded wait, and prints one `PASS` line per pass with the time to retire. When a submission does not
retire, the line names the pass. `--isolate 0` puts all passes in one submission, as before; it is the control.
`--only` also selects what is recorded: a pass that is not selected is not drawn. The vertex reference is always
drawn, because the other passes are compared with it.

Output: the `FEATURES` line carries `MeshShaderTier`, then a `REFERENCE` line, then one `CONFORMANCE` line per
subtest and `CONFORMANCE overall`. Exit codes are the same as the conformance client's: 0 all pass, 1 a failure or
an error, 4 skip only, 2 bad usage, 3 no device or GPU work that did not retire. A tier of 0, or a highest shader
model below 6_5, is a SKIP with a `reason=` and exit code 4: a mesh or an amplification program is a DXIL program
of shader model 6.5. `--out` writes `mesh.json` and `mesh.bin`, which holds the three images as 12288
little-endian UINT32 words.

Host controls, runtime 10.0.26100.9278, exe SHA256 6570A0C5:

```
WARP        FEATURES MeshShaderTier=10 ... HighestShaderModel=6_8 MaxSupportedFeatureLevel=12_1 device=12_1
RTX 4090    FEATURES MeshShaderTier=10 ... HighestShaderModel=6_8 MaxSupportedFeatureLevel=12_2 device=12_1
both        REFERENCE covered=520 least_per_triangle=65 invalid=0
both        CONFORMANCE mesh-ms PASS mismatches=0 checked=4096 control_permuted_ptag=520
both        CONFORMANCE mesh-as PASS mismatches=0 checked=4096 control_permuted_ptag=520
both        CONFORMANCE overall PASS, exit 0, DEBUGLAYER errors=0 warnings=6
```

The two `mesh.bin` dumps are byte-identical (SHA256 7A279529), so the three images agree across a software and a
hardware implementation. The six debug layer warnings are 929, the complement prefill written by the CPU into
READBACK memory, and 820, a clear without a clear value in the resource description. The `-TestForceTier0` arm
reports `CONFORMANCE overall SKIP` with `reason=MeshShaderTier=0,HighestShaderModel=6_8` and exits 4.

## Caveats

- WARP 10.0.26100.9278 sets `SV_InnerCoverage` bit 0 on every conservatively covered pixel of a back-facing
  triangle. The RTX 4090 does not. The decisive passes therefore draw all triangles front-facing, and three of the
  eight are reoriented. The back-facing pass is an observation only. The specification does not distinguish the
  facing, so a back-facing failure on the lab is worth a note and not a verdict.
- The plain-UAV pass is an observation and never a verdict. Hardware may run the pixels in any order.
- Tier 1 conservative rasterization gives no exact expected image. Only tiers 2 and 3 do.
- The interactive exit code is the protocol's. A failing subtest shows as `copy_success` false and a
  diagnostic-restored kit status, not as a non-zero client exit.
- The lab kit's pull step extracts an allowlist of files. A trial script must take `conformance.json` and the
  dumps from the collected archive.
- The ray hit and miss oracle keeps every ray at least 0.05 units from each triangle edge, which the client checks
  at run time. The scene is small, with 12 hits and 52 misses. It exercises the dispatch path and not the quality
  of traversal.
- The mesh witness exercises one mesh group of one triangle per group, the primitive output linkage and the
  amplification payload. It is a witness of the path, not a measurement of mesh shader throughput, and it says
  nothing about per-primitive culling or about the group sizes a game uses.
