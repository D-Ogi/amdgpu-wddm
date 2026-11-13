# shared-interop - D3D11 creates shared resources, D3D12 opens them

`shared-interop.exe` tests the open of D3D11 shared resources by D3D12 on one adapter, in one process (BD-373). It
is a console process. It opens no window and starts nothing resident.

## Build

```
pwsh tools\win\shared-interop\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget [-Out <dir>]
```

The build uses clang-cl and lld-link through `tools\build\umd-compiler.ps1`, as `tools\win\capture-share` does. It
checks `--help` and a bad argument, and writes `SHA256SUMS.txt`. The default output is
`<BC250_ROOT>\scratch\build\shared-interop\shared-interop.exe`.

## What it does

1. It selects the first hardware adapter (or `--adapter <index>`). It creates a D3D11 device and a D3D12 device on
   that adapter and prints the adapter LUID and the two devices. The case `same-adapter` compares the LUIDs.
2. D3D11 creates a shared fence and D3D12 opens it (case `shared-fence`). Both directions use this fence.
3. For each buffer, there are two cases with the legacy handle (`D3D11_RESOURCE_MISC_SHARED`,
   `IDXGIResource::GetSharedHandle`):
   - `<buffer>-kmt-11to12`: D3D11 creates the shared buffer with a pattern as its initial data and signals the
     fence. D3D12 opens the handle, checks `GetDesc`, waits for the fence on its queue, copies the buffer to a
     READBACK buffer and compares every byte.
   - `<buffer>-kmt-12to11`: D3D12 copies a second pattern into the opened buffer and signals the fence. D3D11
     waits for the fence, copies the buffer to a staging buffer and compares every byte.

   The buffers are `buffer-100` (vertex and shader resource binds), `buffer-4096` and `buffer-65536` (shader
   resource and unordered access binds), and `structured-4096-s16` (structured, stride 16).
4. `buffer-4096-nt-create` holds the rule of the D3D11 runtime for the NT handle form: `CreateBuffer` with
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` returns `E_INVALIDARG` before a driver gets the call. The case passes on
   this result with both devices alive. Thus a D3D11 buffer has no NT handle, and the legacy handle is the buffer
   path to D3D12.
5. In `texture-mips-nt-open`, D3D11 creates a shared 64x64 texture with 7 mips and D3D12 opens it. By default the
   open must fail with both devices alive. On a driver that opens such textures (engine ABI 1.11 on an adapter with
   linear mips), `--texture-open-expected` makes the case pass when the open succeeds with 7 mips.

Each byte of a pattern is a function of its index and of the case, so a shifted, short or stale copy shows. A
failed case names the step, the HRESULT, the count of bytes that differ and the first one.

## Output and exit codes

```
ADAPTER luid=<high>:<low> vendor=0x1002 device=0x13fe desc=<adapter description>
DEVICE11 luid=<high>:<low> feature_level=0xb100 desc=<adapter description>
DEVICE12 luid=<high>:<low> desc=<adapter description>
CASE <name> PASS|FAIL <detail>
SHARED-INTEROP PASS|FAIL cases=<n> failed=<n>
```

The program also reads the debugger output of its own process through the DBWIN buffer, as a debug viewer does. It
prints each line as `DEBUG <text>`, at most 200 lines. The line that names why an open was refused is
`DEBUG amdgpu_wddm_d3d12 engine-ddi: OpenHeapAndResource: <hr> reported as <hr>, declined at <step>; ...`. The first
line of the run is `DEBUG-CAPTURE on`, or `DEBUG-CAPTURE off <reason>` when a debugger is attached or another DBWIN
reader runs: then the lines go to that reader. After `DEBUG-CAPTURE on` the program writes one line of its own, which comes back as `DEBUG shared-interop: capture check`: without it, the channel does not work.

Exit codes: 0 every case passed, 1 a case failed, 2 the arguments were wrong, 3 no device of one of the two APIs.

## Bounds

Each GPU wait and each fence wait has a timeout of 5 s. If a D3D12 submission waits for a fence value that does not
come, the program signals that value from the CPU, so the queue does not stay blocked for the next case. A watchdog
ends the run after 55 s with a FAIL line.

The adapter LUID and the device lines let a lab kit match the `DEBUG` lines and other driver logs with this run.
