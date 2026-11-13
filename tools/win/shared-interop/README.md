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
   resource and unordered access binds), and `structured-4096-s16` (structured, stride 16). Two more cases change one
   property of `buffer-100` at a time: `buffer-100-uav` has its size with the binds of `buffer-4096`, and
   `buffer-4096-vb` has its binds with the size of `buffer-4096`.
4. `buffer-4096-nt-create` holds the rule of the D3D11 runtime for the NT handle form: `CreateBuffer` with
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` returns `E_INVALIDARG` before a driver gets the call. The case passes on
   this result with both devices alive. Thus a D3D11 buffer has no NT handle, and the legacy handle is the buffer
   path to D3D12.
5. In `texture-mips-nt-open`, D3D11 creates a shared 64x64 texture with 7 mips and D3D12 opens it. By default the
   open must fail with both devices alive. On a driver that opens such textures (engine ABI 1.11 on an adapter with
   linear mips), `--texture-open-expected` makes the case pass when the open succeeds with 7 mips.
6. D3D12 creates three shared buffers: `d12-buffer-100`, `d12-buffer-4096-uav` and `d12-buffer-65536-uav`. Each
   is a committed buffer on a DEFAULT heap with `D3D12_HEAP_FLAG_SHARED` and an NT handle. The `-uav` buffers allow
   unordered access. The D3D12 shell of this driver refuses a shared buffer today. Thus by default, a create that
   fails with `E_OUTOFMEMORY` passes as `<buffer>-12to12`, and the other cases of that buffer do not run. With
   `--buffer-open-expected` the create must succeed. D3D12 writes a pattern into each buffer. Then there are three
   cases for each buffer:
   - `<buffer>-12to12`: D3D12 opens the handle, checks `GetDesc` and compares every byte. A process has one D3D12
     device for each adapter, so the device that opens the buffer is the device that created it. The open still goes
     to the driver. By default, a refusal passes and an open with different bytes fails. With
     `--buffer-open-expected` the open must succeed.
   - `<buffer>-12to12-back`: only after an open. D3D12 writes a second pattern through the opened resource and
     reads it through the resource that created the buffer.
   - `<buffer>-12to11`: D3D11 opens the handle with `OpenSharedResource1`. The D3D11 runtime refused this open with
     `E_INVALIDARG` on the development PC (the driver of another vendor, 2026-11-13). Its debug layer gave no reason.
     The case passes on this result with both devices alive. If the open succeeds, the case fails and prints the
     D3D11 description.
7. `buffer-create-cost` creates 256 committed buffers of 4096 bytes on a DEFAULT heap and measures the time of each
   create. It prints the mean, the median, the longest time and the time of the first create. The case passes when
   each create succeeds. To find the cost of the record of the D3D12 driver, the lab compares two runs: one with the
   lab switch `engine-layout-buffers` and one without it.

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
prints a line as `DEBUG <text>`, at most 200 lines. By default it prints only the lines that name a refusal or a
failure: lines that contain `declined`, `refus`, `fail`, `error`, `denied`, `invalid` or `not supported`, without
case. It counts the other lines, for example the format lines of the D3D11 device, and gives the count at the end in
the line `DEBUG-CAPTURE hidden <n> lines ...`. With `--debug-all` it prints every line. The line that names why an open
was refused is
`DEBUG amdgpu_wddm_d3d12 engine-ddi: OpenHeapAndResource: <hr> reported as <hr>, declined at <step>; ...`. The first
line of the run is `DEBUG-CAPTURE on (<filter>)`, or `DEBUG-CAPTURE off <reason>` when a debugger is attached or
another DBWIN reader runs: then the lines go to that reader. After `DEBUG-CAPTURE on` the program writes one line of its own, which comes back as `DEBUG shared-interop: capture check`: without it, the channel does not work.

Exit codes: 0 every case passed, 1 a case failed, 2 the arguments were wrong, 3 no device of one of the two APIs.

## Bounds

Each GPU wait and each fence wait has a timeout of 5 s. If a D3D12 submission waits for a fence value that does not
come, the program signals that value from the CPU, so the queue does not stay blocked for the next case. A watchdog
ends the run after 55 s with a FAIL line.

The adapter LUID and the device lines let a lab kit match the `DEBUG` lines and other driver logs with this run.
