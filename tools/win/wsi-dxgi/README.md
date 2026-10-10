# wsi-dxgi - the harnesses behind the DLL shadowing and present-route evidence, and the share cells

A Vulkan WSI on Windows loads `DXGI.DLL`, `DComp.DLL` and `D3D11.DLL` by full System32 path. A game that
ships DXVK or vkd3d-proton has its own `dxgi.dll` next to the executable. The Windows loader resolves a
module's static imports by module name against the modules that are already loaded. A System32 module that
we load by full path can therefore bind to the game's `dxgi.dll` instead.

These two harnesses measure how far that reaches, and whether the composition present route survives it.
They produced the evidence of E56 and the facts M792, M793 and M794. Both are console processes. They open
no window, they start nothing resident, and they run on the development PC. The lab is not involved.

| File | What it is |
|---|---|
| `shadowtest.cpp` | The loader experiment: what a shadowed `dxgi.dll` reaches, and three candidate mitigations |
| `presenttest.cpp` | The present route the WSI needs, run inside three simulated game directories |
| `sharecell12.cpp` | The b27 cross-stack share cell: a D3D12 producer with a RADV consumer |
| `sharecellvk.cpp` | The same pair the other way round: a RADV producer with a D3D12 consumer |
| `sharecell-common.h` | What the two cells have in common: the pattern, the fence schedule, the loaders |
| `build.ps1` | Builds the harnesses and the cells from the NuGet kits (`-Harness all`) |
| `run-shadowtest.ps1` | Runs each `shadowtest` mode in its own process and writes a manifest |
| `run-presenttest.ps1` | Runs each `presenttest` mode in its own process and writes a manifest |

Both sources resolve every D3D, DXGI and DirectComposition entry point with `GetProcAddress`, so the
executables import none of those modules themselves. That is what lets them decide which copy of a module
a call reaches.

The source files keep their exact bytes in this repository. `.gitattributes` holds them out of line-ending
normalization, because the E56 manifests record their SHA-256 sums. `build.ps1` prints those sums.

## The share cells

The DXGI present route of the Vulkan WSI shares two kinds of object between RADV and our D3D12 shell. One is
a D3D12 committed texture that RADV imports as `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`. The other
is a RADV timeline semaphore that D3D12 opens as an `ID3D12Fence`
(`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`). An audit of 2026-10-10 found that nothing had measured
either of them in the direction the route uses: M802 measured a D3D12 producer with a D3D12 and a D3D11
consumer, which is prerequisite coverage of the kernel driver's sharing. These two cells are the smallest
thing that measures the pair, in both directions, over exactly those handle types.

Each round writes a pattern whose every texel is a function of its own coordinates and of the round. A stride
misread therefore appears as a diagonal, a channel-order error appears in one byte of four, and a stale image
appears as the wrong round constant. The producer owns the odd values of the shared timeline and the consumer
the even ones, so a value that arrives on the wrong side cannot be explained away. The consumer waits on its
own queue and not on the CPU, because the route waits that way too. The timeline CPU wait is bounded at 2000 ms. The
same image, allocation and timeline serve every round, which is the image reuse the route does per frame.

For a shared fence, each cell prints the completed value in decimal and hex, the creation flags, and the
device status before and after the open and after a queue signal. The fresh value is read through both APIs.
The fence's device and the queue's device must use the same adapter. Every D3D12 HRESULT is printed,
including enumeration results. An `UNSAFE` line names a timed-out Vulkan wait or a lost device. The lab
runner owns the process deadline: queue and device idle calls have no timeout argument.

Set `AMDGPU_WDDM_LOG` to a file and `AMDGPU_WDDM_DDI_TRACE=1` for each cell to record entry and exit of
the shell's bound DDI slots. A missing failure line alone does not locate a refusal above the driver.

`--selftest` drives the pure rules with no device of either stack, and `--negative-control` inverts every
case. `build.ps1` runs both for each cell. Exit codes: 0 every case passed, 1 a case failed, 2 the arguments
were wrong, 3 this machine offers no device of one of the two stacks, which is a skip and not a pass.

## The shadowtest modes

| Mode | What it sets up |
|---|---|
| `control` | No application-local `dxgi.dll`. The baseline of the loader behaviour |
| `shadow` | The application-local `dxgi.dll` by name first, then System32 `dxgi` and `d3d11` by full path |
| `shadow-rebind` | `shadow` plus a rewrite of the `d3d11` import table and the `dcomp` delay import table to the System32 modules |
| `shadow-actctx` | `shadow` plus an activation context that holds a `loadFrom` to System32 |
| `shadow-search` | `shadow` plus `LOAD_LIBRARY_SEARCH_SYSTEM32` on the load of `d3d11` |
| `sysfirst` | System32 `dxgi` by full path first, then the same name again by name |

## The presenttest modes

A mode name is a scenario and a policy. The scenario selects the application-local DLLs. The policy selects
the loader behaviour.

| Scenario | Application-local DLLs |
|---|---|
| `control` | None. A pure Vulkan game |
| `dxvk11` | DXVK `d3d11.dll` and `dxgi.dll`. A DXVK D3D11 game |
| `vkd3d` | DXVK `dxgi.dll` and vkd3d-proton `d3d12.dll` with `d3d12core.dll`. A D3D12 game |

| Policy | What it does |
|---|---|
| `plain` | The route as the DXGI WSI would run it. System32 modules by full path and an explicit adapter |
| `seal` | `plain` plus the proposed seal. It rebinds the import tables of the System32 graphics modules to System32 modules that are already loaded, and it redirects bare-name loader calls made from those modules. It loads nothing itself |

The route is one System32 `CreateDXGIFactory2`, `EnumAdapterByLuid` on the wanted LUID, a presenter and a
producer D3D11 device on that adapter, an NT shared texture, a cross-device shared fence, a DirectComposition
device and visual with no target, `CreateSwapChainForComposition`, four frames of `CopyResource` and
`Present1` against the fence, `ResizeBuffers`, and one `ALLOW_TEARING` swap chain. The harness traces every
`LoadLibrary` and `GetModuleHandle` call that a System32 graphics module makes.

## Build and run

```
pwsh -NoProfile -File tools\win\wsi-dxgi\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
pwsh -NoProfile -File tools\win\wsi-dxgi\run-shadowtest.ps1 -Exe <build>\shadowtest.exe `
     -Out <BC250_ROOT>\scratch\m16\wsi-dxgi\logs -AppLocalDxgi <package>\dxgi.dll
```

`-Out` of `build.ps1` defaults to `<BC250_ROOT>\scratch\build\wsi-dxgi`. The workspace root comes from
`BC250_ROOT`, and else from `-Kits`, which points at `<BC250_ROOT>\toolchain\nuget`. Neither script guesses
the root from its own place in the file system, because a build from a git worktree sits somewhere else.
The flags are `/std:c++20 /EHsc /W4 /WX /MD /O2`, which are the flags of the E56 builds.

The application-local DLLs are copies of builds that live in the workspace, never in this repository. Each
runner pins the copy it was given by SHA-256 and stops when the hash does not match, so a result cannot
claim a build it did not use. `run-presenttest.ps1` skips a scenario whose DLLs it did not get, so one run
can cover a subset.

A run writes one log per mode, one directory of DXVK logs per mode, and a `manifest.json` with the harness
hash, the application-local hashes, the System32 module versions, and the exit code and wall time of each
mode. Keep that manifest with the logs. It is the record of what the numbers came from.

The two runners take their paths as parameters. The E56 runs of 2026-09-28 used the same logic from the
workspace design directory with the paths written into the script.

## Limits of this instrument

The measurements of E56 ran against the development PC's own user-mode driver, with no window and therefore
no `CreateTargetForHwnd`, in synthetic application directories and not in a real game. A present interval
measured here is the vsync pace of that machine. None of it is evidence about our driver or about unit A.

The import and export offsets that M794 records come from Microsoft binaries and can move with a Windows
update. The WSI gate must read the bindings at run time. The listings behind those offsets stay outside this
repository.

### Fence origin control (BD-105)

Both cells accept `--fence-origin radv|d3d12`. The `radv` option remains the default and preserves the
RADV-export baseline. With `d3d12`, the cell creates a D3D12 fence with initial value 0 and
`D3D12_FENCE_FLAG_SHARED`, exports an unnamed NT handle with `GENERIC_ALL`, and opens that
same handle on the same D3D12 device. It records the created and reopened fence flags and
values before Vulkan import. This is a separate D3D12-to-D3D12 classification control even
if the subsequent Vulkan import fails. This adapter control expects `SHARED` on both fences.
A successful control does not by itself prove the cause of the RADV-export failure.

RADV then imports that handle permanently as `D3D12_FENCE` into a timeline semaphore. The
application closes its NT handle after import on both success and failure. Import does not
transfer ownership. The reopened D3D12 fence and the Vulkan semaphore remain alive through
the existing odd/even signal, wait and image checks. The texture path and wait bounds stay
unchanged. Use a semaphore implementation with the owned-import fix from Mesa fork commit
`50787ae9` or its descendant. The old importer retained the caller's handle without a duplicate.
that known defect can invalidate this control after the application correctly closes its handle.

The host selftest now has 19 checks at default dimensions. It covers both origin options,
refuses missing/unknown origins, and invokes the production import wrapper with mocked
callbacks. Both successful and failed imports must preserve the result, use a permanent
handle-based D3D12 import, and close the caller handle exactly once after the import call.
These checks do not prove cross-API synchronization. That requires the bounded lab cells.

Contracts: [Vulkan Win32 semaphore import](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#vkImportSemaphoreWin32HandleKHR)
(local Vulkan-Docs `01aaacd9`, synchronization.adoc 5033-5037 and 5087-5115), and
[D3D12 CreateSharedHandle](https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle)
(local sdk-api-docs `a4fd3f7e`, parameters Access and pAttributes).
