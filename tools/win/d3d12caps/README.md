# D3D12 capability dump

`amdgpu_wddm_d3d12caps.exe` writes one JSON document with everything a D3D12 application can learn about an
adapter and its device before it renders. Two routes on the same hardware are compared by running it in each and
diffing the documents with `diff-caps.py`. It opens no window, creates no swap chain and records no command list;
the only objects it creates are the DXGI factory, the device and three empty command queues (for their timestamp
frequency).

Motivation: The Witcher 3 (DX12, Agility SDK) renders through the per-application route but misbehaves on the
native route while no driver call fails. A capability answer that differs between the routes can steer an engine
onto a different path; this tool puts all of them side by side.

## Build

```
pwsh -File build.ps1 -Kits <workspace>\toolchain\nuget                          # plain variant
pwsh -File build.ps1 -Kits <workspace>\toolchain\nuget -AgilitySdkVersion 619    # Agility SDK variant
```

PowerShell 7: under Windows PowerShell 5.1 the bad-argument check stops the script, because the tool's message on
stderr becomes a terminating error there.

Output defaults to `<workspace>\scratch\m15\d3d12caps\build` (`-Out` to change): `amdgpu_wddm_d3d12caps.exe` and
`amdgpu_wddm_d3d12caps_agility<n>.exe`. Static CRT, so the lab needs no runtime install. A rebuild keeps the
previous binary under `retained\` by hash. The script checks on the artifact that the Agility variant exports
`D3D12SDKVersion` and `D3D12SDKPath` (`.\D3D12_0\`) and the plain one does not, that neither `d3d12.dll` nor
`dxgi.dll` is imported statically, the help and bad-argument exits, and runs the Python tests (`test_*.py`).

The game's own value: `witcher3.exe` of The Witcher 3 5.0 exports `D3D12SDKVersion = 619` and
`D3D12SDKPath = ".\D3D12_0\"` (read from the PE exports, not by running it); its `D3D12Core.dll` has file version
1.619.4.0.

## Usage

```
amdgpu_wddm_d3d12caps.exe [adapter-index] [output-path] [--progress]
```

The adapter index follows `IDXGIFactory1::EnumAdapters1` (default 0; WARP is listed as
"Microsoft Basic Render Driver"). Without `output-path` the document goes to stdout; with it, the document is
written to `<path>.tmp`, flushed and renamed over `<path>`. `--progress` names each section on stderr before it
starts, so a crash inside a driver call leaves the section name behind. Exit 0 when the document was written, 1
when writing failed, 2 for a bad command line; every failed API call is data inside the document.

`d3d12.dll` and `dxgi.dll` are loaded by name with the default search order, so a copy next to the executable
wins over System32, exactly as for a game:

- Per-application route: put the executable next to the vkd3d-proton `d3d12.dll` (and `dxgi.dll` if the game
  uses one) in a scratch directory. The Agility exports are ignored there.
- Native route: run it in a directory with no replacement DLLs; the system `d3d12.dll` loads our user-mode driver.
  For the game's runtime core, use the Agility variant with the game's `D3D12Core.dll` in `.\D3D12_0\` next to
  the executable. Without that core, device creation fails with `D3D12_ERROR_INVALID_REDIST` when the system core
  is older than the requested version (seen on a development PC with system SDK version 616).

`modules` in the document tells which `d3d12.dll`, `d3d12core.dll` and driver DLLs actually answered.

## Output schema

Keys are sorted, one leaf per line; lists are objects with zero-padded keys. Every call is recorded with its
`hr` (`0x... NAME`) and, when it succeeded, every field of its result by the field name the SDK headers use.

- `tool`: headers used, Agility SDK version exported (or null), adapter index.
- `dxgi`: factory entry point and hr, `PRESENT_ALLOW_TEARING`, and each adapter's `GetDesc1` (no LUID),
  `GetDesc3` flags and preemption granularities, and the UMD version from `CheckInterfaceSupport(IDXGIDevice)`.
- `adapter`: the selected adapter's `QueryVideoMemoryInfo` (LOCAL, NON_LOCAL) and each output's `GetDesc1`
  (no device name), hardware composition support and display mode counts with the largest mode per format.
- `device`: `D3D12CreateDevice` from 12_2 down to 11_0 and `created_at`; `features` (every `D3D12_FEATURE` the
  headers define, OPTIONS to OPTIONS21, FEATURE_LEVELS as a full list, a graphics list and per level, SHADER_MODEL
  and ROOT_SIGNATURE probed from the newest down, COMMAND_QUEUE_PRIORITY per type and priority,
  PLACED_RESOURCE_SUPPORT_INFO per heap type); `raw_features` (values 54-79, past the headers, with the first
  structure size the runtime accepts and the raw bytes); `formats` (FORMAT_SUPPORT with decoded flag names,
  FORMAT_INFO plane count, MULTISAMPLE_QUALITY_LEVELS 1/2/4/8 for render target and depth formats, for DXGI_FORMAT
  0-132 and 189-191); `allocations` (GetResourceAllocationInfo and GetCopyableFootprints for typical game
  resources, including small-alignment requests); `properties` (node count, descriptor increments, custom heap
  properties, QueryInterface for ID3D12Device1-14 and the device configuration, meta commands, queue timestamp
  frequencies); `GetDeviceRemovedReason` at the end.
- `modules`: every module loaded after start, by base name, with its location (`exe_dir...`, `system32`,
  `driver_store\<package>`, `other`) and file version.

## Comparing

```
python diff-caps.py native.json per-app.json [--ignore REGEX ...]
```

prints `path: left -> right` for each differing leaf, sorted, with added and removed names for the `...Names`
flag lists, and the count on the last line. Exit code 0 always. Memory budgets and usage change between runs;
`--ignore "QueryVideoMemoryInfo"` hides them.

## Acceptance

```
python check-caps.py caps.json [caps.json ...]
```

checks the allocation answers a game sizes its heaps with: every case of the tool's table present (the
inventory is listed in the script), exactly 65536 / 65536 for the 64 KiB buffer, a positive size that is a
multiple of a power-of-two alignment for every other resource and at least the width for a buffer, 64 KiB
(4 MiB for MSAA) when no alignment was requested and the requested one otherwise, a positive footprint total
for every single-sampled case, and `UINT64_MAX` for the deliberately invalid
`small_rgba8_256_align4k_too_large`. Exit 0 when every document passes, 1 on a failed check, 2 when a document
cannot be read. The dump tool's own zero exit proves only that the JSON was written: on the native route every
size once came back as the negated alignment while no driver call failed. Run the check on each document
separately; a document from a route whose device creation failed (for example `D3D12_ERROR_INVALID_REDIST`
without the game's core) has no allocations and fails here by design, so record it apart from the API results.

## Allocation refusal probe

`amdgpu_wddm_d3d12allocprobe.exe` asks whether a texture the driver cannot size costs the application its device.
Our engine has no layout for YUY2 and R8G8_B8G8_UNORM. The shell used to report `E_INVALIDARG` through the device
error callback (`pfnSetErrorCb`) when `CheckResourceAllocationInfo` was asked about one; it now answers
`ResourceDataSize` `UINT64_MAX`, the API's error answer, and reports nothing. The D3D12 DDI reference names no error
for that function. The rules for that callback ("Handling Errors",
learn.microsoft.com/windows-hardware/drivers/display/handling-errors, written for the D3D10 DDI) make an error a
function does not allow critical: the runtime removes the device. Whether the D3D12 runtime asks the driver about
these formats at all, and what it makes of either answer, is what the probe measures.

For R8G8B8A8_UNORM (the control), YUY2, R8G8_B8G8_UNORM and R8G8B8A8_UNORM again, it runs four steps on a 64 x 64
2D texture (one mip, one sample, layout UNKNOWN, no flags, state COMMON): `GetResourceAllocationInfo`,
`CreateCommittedResource` on a DEFAULT heap, `CreateHeap` (4 MiB, DEFAULT, `ALLOW_ONLY_NON_RT_DS_TEXTURES`) and
`CreatePlacedResource` at offset 0. `GetDeviceRemovedReason` is read right after each step. The format's
`FORMAT_SUPPORT` answer is read last. Before each format, a removed device is released and a new one created at the
same level (`0_new_device`), so one removal does not hide the later answers.

```
pwsh -File build.ps1 -Kits <workspace>\toolchain\nuget -Tool d3d12allocprobe [-Out <dir>]
amdgpu_wddm_d3d12allocprobe.exe [adapter-index] [output-path] [--progress]
```

The command line and the document's format are those of the dump. `device.created_at`,
`device.D3D12CreateDevice`, the level list and tiled tier under `device.features`, `device.GetDeviceRemovedReason`
(of the last device, at the end) and `modules` carry the dump's key names. A trial's caps profile can therefore run
the probe in place of the dump. The results are under `probe.<n>_<format>.<step>`: `hr`, the sizes
(`SizeInBytes` and `Alignment`, `UINT64_MAX` being the runtime's error marker) and `GetDeviceRemovedReason`.
Unlike the dump, the probe loads `d3d12.dll` and `dxgi.dll` from System32 only, so the system runtime and the
registered driver answer whatever lies next to it.

Exit 0 when the document was written, a removed device included; 1 when writing failed; 2 for a bad command line;
3 when a step did not return within 12 s. In that last case a deadline thread writes the document as it was, with
`deadline.hit` and the step that did not return, and ends the process. The build checks that path by stopping the
main thread in its first step (`D3D12ALLOCPROBE_TEST_STALL=dxgi`), before any device is created.

Diabeł tkwi w szczegółach - the devil is in the details, and here they are all on one page.
