# Design: the Vulkan WSI presents through DXGI on a D3D12 device

Date: 2026-10-07. Ported to the b23 release line on 2026-10-08 and to the shipped b25 system ICD line on
2026-10-09. Status after the first lab trial of 2026-10-09: **the route has never presented a frame on unit A.
It froze every client that used it before its first present, with the GPU idle and no TDR (BD-105), so the
route is opt-in and GDI is the default again.** GDI is also the explicit rollback and the automatic fallback
when the DXGI route fails. The owner's decision of 2026-10-07 (a route that must be switched on gets
forgotten) returns in the train whose lab arm presents a frame through this route.

What the first trial measured, and what the branch does about it, is the section
[Deadlines and the retired route](#deadlines-and-the-retired-route-bd-105).

This route is how the M15.14 criterion is met for Vulkan. The owner put Vulkan into M15.14 on 2026-10-08
(independent flip: the display pipeline scans out a fullscreen or borderless game from the game's own
swap-chain buffer, and DWM does not compose its frames). The registered system Vulkan ICD presents with `D3DKMTPresent` and a blit, so DWM
composes every Vulkan frame today. A DXGI flip-model chain on a D3D12 device of our adapter has the shape the
D3D12 shell's scan-out rule admits, so the Vulkan chain can reach the same flip as a native D3D12 game.
Step 5 and step 7 of the lab plan are that measurement. See the M15.14 row of
[the M15 reconciliation](../m15-reconciliation.md).

**Two ICD lines, and this work sits on one of them.** The release carries two 64-bit RADV builds. The D3D ICD
line (the triplet line) belongs to our D3D11 and D3D12 shells, which load it in hosted mode. There the shell
presents and the ICD has no WSI role at all. The system Vulkan ICD line is
`payload/vulkan/vulkan_radeon.dll`, which the Vulkan loader gives to every pure Vulkan process. This WSI work
belongs to the second line. The branch for the trial is `amdgpu-wddm/b26-vk-wsi-dxgi`: the shipped system ICD
line of 0.7.216.100-tester.23 (`amdgpu-wddm/b25-system-icd` `d3da6d0a`), the six route commits, and one commit
that gives this line the log stream the route lines write to. The candidate therefore differs from the
installed file by the route and by that log stream, and by nothing else. The port keeps the three fixes that
line already ships: the Windows shader disk cache, the BD-102 BVH node address, and the fence-wait shape that
reads the device state only when the wait needs it.

This note follows [ADR 0018](../adr/0018-engine-present-for-vulkan-wsi.md) and replaces the
D3D11 and KMT plans of [the engine present note](wsi-engine-present.md) for the WSI side. Nothing here is a
measured result unless it cites a `facts.md` row.

## The problem

A pure Vulkan application (vkcube, a Vulkan game) presents through the WSI of our RADV ICD. Today that WSI takes
Mesa's CPU-image path: the image is copied on the CPU into a DIB, `BitBlt` puts it in the window and `DwmFlush`
waits for the compositor. On a real scene the present took 15.8 ms of a 33.4 ms frame (facts M580). ADR 0018
retired that transport as the default. The kernel-thunk route of the first ADR 0018 plan does not work:
dxgkrnl refuses a windowed `D3DKMTPresent(Blt)` from the ICD at admission (facts [M598](../facts/display.md#m598),
[M601](../facts/display.md#m601), [M677](../facts/display.md#m677)).

The aim is a present that copies only on the GPU, composes in a window, and can reach independent flip in
fullscreen (M15.14), with room for 10-bit and HDR.

## Three routes

| | (a) Mesa's DXGI WSI over a D3D device of our adapter | (b) RADV scan-out primaries, presented by a composition swapchain | (c) Reuse of the D3D12 shell's present |
|---|---|---|---|
| Mechanism | The upstream path of `wsi_common_win32.cpp`. A flip-model DXGI swap chain runs on a D3D12 queue. Each present does one `CopyResource` from the shared resource under the Vulkan image, ordered by shared fences | RADV allocates the swapchain images as scan-out primaries with the E26R record. The WSI gives them to `IPresentationManager` or to a DXGI wrapper of our own | The Vulkan swapchain feeds the present code of `amdgpu_wddm_d3d12.dll` directly |
| What exists | The whole user-mode path is in upstream Mesa. A native D3D12 device on our adapter is the default for games (trials 053-056, M15.1). D3D12 shared resources and cross-API sharing work on unit A on the GPU route ([M802](../facts/d3d.md#m802)). The D3D12 shell describes a shared resource as an LB7A linear surface ([D3D12 shared resources](../d3d12-shared-resources.md)), and RADV imports LB7A | Nothing. RADV writes no E26R record, and no public API wraps foreign allocations into an `IDXGISwapChain` | The shell presents only for a D3D12 device and a DXGI swap chain that the runtime created. Its present is a DDI behind the D3D12 runtime and dxgkrnl, not a call another module can make |
| Window composition | Yes, a GPU copy into the swap chain buffer | Yes | Only through (a) |
| Independent flip | Yes in principle. The swap chain has the shape of a native D3D12 game, so it inherits M15.14 increment 2 (main `3eca5f98`). That is the shell's E26R v3 scan-out record under `AMDGPU_WDDM_D3D12_EXPERIMENT=scanout-flip-1920x1200`, and the answer of the router front to `pfnCheckDirectFlipSupport` | No. Direct scan-out and iflip of the composition swapchain API need WDDM 3.0 (`ref/win32-docs/desktop-src/comp_swapchain/comp-swapchain.md`, lines 30 and 41). The kernel driver is WDDM 2.9 (`DXGKDDI_INTERFACE_VERSION` 0xE003, `driver/kmd/bc250kmd.h`). The API also takes a D3D11 device, and the D3D11 shell writes v3 without SCANOUT ([M15.14 row](../m15-reconciliation.md)) | Through (a) |
| Second device | A D3D12 device on the same adapter in the Vulkan process: our D3D12 shell with its hosted RADV | A D3D11 device | Same as (a) |
| New code | WSI glue and driver hooks | A new present transport, a new record writer in RADV, and a WDDM 3.0 kernel driver for any flip | Same as (a) |

Route (c) is route (a) with D3D12: the only way to reach the shell's present is a D3D12 device and a DXGI swap
chain, and that is what the DXGI WSI makes. Route (a) with a D3D11 device (the
[M16 draft](wsi-engine-present.md)) would use the D3D11 shell, whose fence support for this use is not
measured, and whose primaries carry no SCANOUT bit. Route (b) gives composition only on this kernel driver and
needs the most new code.

**Decision: route (a) on a D3D12 device, which is route (c).** The reasons, in order: every part of the path
already runs on unit A in the native D3D12 games. The swap chain has the shape M15.14 works on, so independent
flip needs no work of its own here. The change is a small set of hooks in Mesa, behind a switch.

## How a frame moves

1. When the application lists the surface formats, and again at swapchain creation, the WSI asks the driver
   whether the route can run (`route_allowed`). The driver answers no only when it cannot make its D3D12
   presenter, so the formats that have no CPU path are not offered then. Then the WSI creates
   one D3D12 shared committed resource per image (`D3D12_HEAP_FLAG_SHARED`). Our D3D12 shell makes it a linear
   surface in the aperture and writes its LB7A record.
2. The Vulkan image is created with `VK_IMAGE_TILING_LINEAR` and imported over that resource
   (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`). RADV's import checks the LB7A block and keeps its
   pitch in the BO metadata. The driver then compares the image's own linear pitch and size with the BO
   (`check_blit_image`). Both sides compute the layout with the same RADV surface code, so a mismatch means two
   different builds, and the swapchain falls back instead of sheared rows on the screen.
3. The application renders into the image. At present, the Vulkan queue signals a timeline semaphore that is
   exported as a D3D12 fence. The D3D12 queue waits for it, runs `CopyResource` into the DXGI back buffer,
   signals the next value, and the Vulkan side waits for that value before it signals the application's
   fences. Then `Present1`.

There is no CPU pixel copy on this path. The present log (`BC250_WSI_PRESENT_LOG`) writes `copy_us` 0 and
`dwmflush_us` 0 for it, because the copy is on the GPU and the completion wait moved to `wait_for_present`.

### The second device: cost, recursion, memory

- **Recursion.** The presenter takes the adapter from a System32 DXGI factory by its LUID and calls
  `D3D12CreateDevice` from System32 `d3d12.dll`, loaded by full path. The runtime loads our D3D12 shell. The shell loads its engine and its hosted RADV (`amdgpu_wddm_radv.dll`) with `LoadLibraryExW`
  by full path (`driver/umd/d3d12/adapter-caps.cpp`), not through the Vulkan loader, so the call never comes
  back into `vulkan_radeon.dll`. The hosted RADV runs with the host dispatch set, and the winsys keeps the route
  off there, so the shell's own instance never makes a D3D12 device either.
- **When.** The device is made when the process first lists surface formats or makes a swapchain, once per
  winsys, under a lock. A failure is
  remembered, so a game that recreates its swapchain does not retry the device each time.
- **Cost.** Not measured. The lab plan measures the time of the first swapchain creation and the process's
  private bytes before and after it. The device holds one direct queue, two command lists and two committed
  resources per swapchain.

## Flips

| Situation | What the route gives | Evidence still needed |
|---|---|---|
| Window, DWM composes | A flip-model swap chain (`FLIP_DISCARD`) whose buffers DWM composes. The one copy per frame is a GPU copy | ETW: present-history tokens with model REDIRECTED_FLIP and `Present` flags 0x9000 for the Vulkan process |
| Fullscreen or borderless at the output's size | The same swap chain is the candidate for DirectFlip and independent flip, as for a native D3D12 game. It needs the M15.14 increment 2 experiment at the source geometry (1920x1200) | The front's `answer=1 rule=supported` line and the analyser's `M15.14 INCREMENT2` verdict |
| Exclusive fullscreen | Not used. The WSI calls `MakeWindowAssociation(NO_ALT_ENTER | NO_WINDOW_CHANGES)` and never calls `SetFullscreenState` | - |

The swap chain is created for the window (`CreateSwapChainForHwnd`) by default, because that is the shape the
native games present with and the one the M15.14 analysis covers. Upstream's composition swap chain with a
DirectComposition visual stays available as `dxgi-composition`.

Only one flip-model swap chain may belong to a window. When the application passes `oldSwapchain` for the same
window, the new chain takes the old chain's swap chain: the D3D12 queue is flushed, the old back buffers and copy
lists are released, and `ResizeBuffers` keeps the swap chain when its flags match. Otherwise the old swap chain is
released and a new one is created. The old chain is then detached: its presents return
`VK_ERROR_OUT_OF_DATE_KHR`.

## Present modes

| Vulkan mode | `Present1` | Behaviour |
|---|---|---|
| FIFO | sync interval 1 | One frame per refresh |
| MAILBOX | sync interval 0 | The newest frame replaces a queued one |
| IMMEDIATE | sync interval 0, `DXGI_PRESENT_ALLOW_TEARING` | Tearing only when `IDXGIFactory5::CheckFeatureSupport` reports it. The swap chain then carries `ALLOW_TEARING` from creation. Without it, IMMEDIATE acts as MAILBOX. The kernel driver has no immediate flip, so a tearing flip still waits for vertical blank |

Every chain is created with `ALLOW_TEARING` when the system reports it, whatever its mode, so the mode can change
per present (`set_present_mode`) and a chain that takes over a window's swap chain keeps the flags
`ResizeBuffers` needs. `vkQueuePresentKHR` never waits for DWM on this route: `DwmFlush` runs in
`vkWaitForPresentKHR` only.

## Formats, 10-bit and HDR

The format table (`wsi_common_win32.cpp`) gives each Vulkan format the format of the shared resource and of the
swap chain buffer:

| Vulkan format | Color space | Buffer | Route |
|---|---|---|---|
| B8G8R8A8_UNORM, R8G8B8A8_UNORM, B8G8R8A8_SRGB | sRGB nonlinear | BGRA8 or RGBA8 UNORM | DXGI and GDI |
| R8G8B8A8_SRGB | sRGB nonlinear | RGBA8 UNORM | DXGI only |
| A2B10G10R10_UNORM_PACK32 | sRGB nonlinear | R10G10B10A2_UNORM | DXGI only |
| R16G16B16A16_SFLOAT | extended sRGB linear (scRGB) | R16G16B16A16_FLOAT | DXGI only, with `VK_EXT_swapchain_colorspace` |
| A2B10G10R10_UNORM_PACK32 | HDR10 ST2084 | R10G10B10A2_UNORM | DXGI only, with the extension, when the window's output is in HDR |

A flip-model swap chain takes no sRGB buffer format, so sRGB images use UNORM buffers and the Vulkan view does
the encoding. The DXGI color space follows the swapchain (`SetColorSpace1`), also back to sRGB, and
`vkSetHdrMetadataEXT` reaches `IDXGISwapChain4::SetHDRMetaData`. The LB7A import takes the pixel size from the
format (4 bytes for BGRA8, RGBA8 and RGB10A2, 8 for RGBA16F), the same rows the contract table admits for a
shared surface (`driver/contract/amdgpu_wddm_surface_format.h`). Nothing in the route assumes 8 bits. Scan-out of
10-bit and FP16 primaries is a matter for the kernel driver and the shell's scan-out rules, not for the WSI. The
contract table admits RGB10A2 for composition and for a scan-out primary, and RGBA16F for composition only.

**No lab run covers the four rows below the 8-bit rows.** The lab plan runs vkcube, which takes the first format
the surface reports, and that is an 8-bit one. These rows are the readiness the owner asked for on 2026-09-29, not
a tested capability, and the release notes must say so until a client asks for a named format.

## The switch and the fallback

- `AMDGPU_WDDM_VK_WSI` in the environment, else the REG_SZ value `WsiRoute` under
  `HKLM\SOFTWARE\amdgpu-wddm\Vulkan`: `gdi`, `dxgi` (window swap chain) or `dxgi-composition`. An empty
  environment value counts as absent. Any other value selects GDI and the log names it invalid.
- The registry read asks for the 64-bit view (`RRF_SUBKEY_WOW6464KEY`) on a 32-bit image. The installer and
  the control application write that view only, so a 32-bit ICD without the flag would read the empty
  `WOW6432Node` copy and the machine-wide rollback would not reach a 32-bit Vulkan application. The D3D12
  shell and the DXVK front ask the same way (`driver/umd/d3d12/ddi-trace.h`,
  `driver/umd/dxvk/scanout-primary.h`). No arm measures this yet: step 2 runs the 64-bit client, and the
  x86 candidate has no arm.
- The default is `gdi` (`RADV_WDDM2_WSI_ROUTE_DEFAULT`), so a process reaches the DXGI route only by asking
  for it. It was `dxgi` until the first lab trial froze every client of the route (BD-105). A route that
  hangs must not be the one every Vulkan application takes without asking. `gdi` from either source is also the
  rollback, as ADR 0018 point 2 asks, and an invalid value selects GDI too, so a mistyped rollback still
  rolls back.
- **Application-local runtime modules do not change the route.** The presenter binds System32 `dxgi.dll` and
  `d3d12.dll` by full path and takes `CreateDXGIFactory2` and `D3D12CreateDevice` from those handles. It never
  looks up a runtime module by name. `util_load_system_library` loads with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` and
  `LOAD_LIBRARY_SEARCH_SYSTEM32`, so a dependency that is not loaded yet comes from System32. The DXGI present
  route passes beside DXVK and vkd3d-proton DLLs in all six E56 modes ([M793](../facts/d3d.md#m793)). The
  shadow of [M792](../facts/d3d.md#m792) and [M794](../facts/d3d.md#m794) breaks only the D3D11 calls of
  `D3D11CreateDevice` with a NULL adapter and the `GetModuleHandle("dxgi")` and `CompatValue` lookup. This route
  is D3D12 and uses an explicit adapter. The init line names each copy, `loaded` when it is in the process,
  `file` when it is only in the executable's directory, and adds `bound=System32`. An Agility SDK
  `D3D12Core.dll` is Microsoft's newer runtime over our user-mode driver, so it is no reason to leave the route.
- **One narrow fallback: a replacement D3D12 core.** System32 `d3d12.dll` (10.0.26100.9278) does not import
  `d3d12core.dll`. It holds the name as a string and loads its core at run time. vkd3d-proton's `d3d12core.dll`
  exports the same `D3D12GetInterface` and `D3D12SDKVersion` (`libs/d3d12core/d3d12core.def`). If the System32
  runtime binds to that module, the "D3D12 device" is vkd3d-proton on Vulkan. It has no LB7A shared resources,
  and it re-enters this ICD. After `D3D12CreateDevice`, the presenter finds the module of the device's vtable.
  A `d3d12core.dll` or `d3d12.dll` outside System32, and outside the directory that the executable's
  `D3D12SDKPath` export names, is a replacement. The process then presents through GDI with the reason
  `d3d12-replaced`. A different module name, such as the debug layer `d3d12SDKLayers.dll` or a capture tool,
  wraps the runtime and is accepted. Whether the System32 runtime binds to such a core at all is not
  measured. The check acts only on the device it actually got.
- **Automatic fallback, each with its HRESULT in the log.** On the init line: the DXGI runtime could not be set
  up (`dxgi-load`, `dxgi-factory`, `dcomp-load`, `dcomp-device`). On the `D3D12 presenter device` line:
  `dxgi-adapter`, `d3d12-load`, `d3d12-device`, `d3d12-replaced`, `d3d12-queue`. On the swapchain lines: a
  fence or resource import, the layout check, or the swap chain itself. In each case that swapchain takes CPU
  images, and the log line names the reason. A format that has no CPU path is offered only while the presenter
  works. If the presenter fails between the format list and the swapchain, such a format fails with
  `VK_ERROR_INITIALIZATION_FAILED`.
- The native D3D11 and D3D12 games do not use this WSI at all: they present through DXGI and our shells. The
  hosted RADV inside the shells keeps the route off.

## Deadlines and the retired route (BD-105)

The first lab trial of this route, on 2026-10-09, froze every client that used it. Quake II RTX stopped during
renderer init and vkcube after its swap chain was made. Both processes stayed alive to the bound of their arm
and presented nothing. The GPU was at its 500 MHz idle state throughout, the flip count was the idle desktop
rate, and there was no GPU fault, no fence timeout and no TDR at `TdrDelay=10`. A packet that never completes
stops Windows within ten seconds, so 120 seconds without a TDR proves that no packet was outstanding: the
block was a CPU wait with no GPU work behind it. The blocking call is not named yet. The measurement that
names it is a user-mode stack of the frozen client, which needs the lab.

Whatever that call turns out to be, a wait of this route may not be unbounded. The rules are plain C in
`src/vulkan/wsi/wsi_win32_deadline.h` of the Mesa fork, with no Windows or Vulkan header, so the host test of
this lane drives the production rules:

- **One deadline, two seconds** (`WSI_WIN32_ROUTE_DEADLINE_NS`). A present of this route costs under two
  milliseconds of CPU at 1920x1200 by the b26 present log, and a lab trial has three minutes, so two seconds
  separates a slow frame from a dead route with room on both sides.
- **The queue flush waits on an event.** `wsi_win32_flush_d3d12_queue` signalled a fence and then called
  `SetEventOnCompletion(1, NULL)`, which blocks the calling thread with no deadline. It now waits on a real
  event to the deadline, logs the expiry and goes on with the release. The swapchain is being torn down
  either way, and the alternative is a thread that never returns. With no event to create, it polls the
  fence to the same deadline.
- **The first acquire caps the application's timeout.** An application asks `vkAcquireNextImageKHR` for
  `UINT64_MAX`. While the route has not completed one present, that becomes the route's own deadline, and on
  expiry the swapchain is reported `VK_ERROR_OUT_OF_DATE_KHR`, which is an answer every application handles:
  it creates the next swapchain. Once a present has completed, the application's timeout is its own business
  again, because the route is then known to work.
- **A wait that expires before the first present retires the route for the process.** The next swapchain of
  that instance takes CPU images with the reason `route-deadline`, so the application keeps a window instead
  of a frozen thread, and nothing re-arms the route inside the process. A late wait after one present has
  completed is a slow frame and changes nothing.
- **The D3D12 queue's own `Wait()` takes no deadline.** It is a GPU-side wait, and it is covered from the
  other end, because the acquire that waits for that copy is bounded now.
- **A first present logs the stage it enters**: `images` (the swapchain's images and their D3D12 blit
  contexts are ready), `queue-wait`, `execute`, `queue-signal`, `present1`, `present1-returned`. One line
  each, for the first present of the route only, so a freeze names the call that did not return without a
  debugger on the machine. The b26 evidence stopped at the swap chain line, which left the image creation and
  the whole first present open as one window.

What this does not do: it does not make the route present. If the freeze is still there, the route now ends
as a bad frame rate and a named stage instead of a dead game, which is the difference between a lane
experiment and a release defect.

## Code

| Repository, branch | Change |
|---|---|
| Mesa fork, `amdgpu-wddm/b26-vk-wsi-dxgi`, which is the shipped system ICD line `amdgpu-wddm/b25-system-icd` `d3da6d0a`, the six route commits and the log-stream commit. `amdgpu-wddm/vk-wsi-dxgi-b23` and `amdgpu-wddm/vk-wsi-dxgi` are the same work on the earlier b23 system line and on the D3D ICD line | `radv_wddm2_wsi_route.h` and its host test: the switch, the report of application-local modules, the D3D12 implementation check, the LB7A rules |
| | `radv_wddm2_bo.c`: the import of 10-bit and FP16 surfaces |
| | `radv_wddm2_wsi.c`: the hooks, the presenter device with the System32 binding, the layout check |
| | `wsi_common.c`: the blit hook of each swapchain and the CPU wait |
| | `wsi_common_win32.cpp`: the route, the init failure and its HRESULT |
| | `u_win32_library.h`: the search for dependencies in System32 only |
| | `amdgpu_wddm_stdio.h` and `u_amdgpu_wddm_stdio.c`: the named log stream the route lines use. On the D3D ICD line that pair also redirects the whole process's stdio. On the system line it declares the log and nothing else. The ICD's stdio therefore stays as the registered build has it |
| Mesa fork, `amdgpu-wddm/b27-vk-wsi-dxgi`, which is `amdgpu-wddm/b26-vk-wsi-dxgi` plus the answer to BD-105 | `wsi_win32_deadline.h`: the deadlines, the retired route and the stage names, as plain C the host test drives |
| | `wsi_common_win32.cpp`: the bounded queue flush, the capped first acquire, the stage lines, the refusal of the route after an expired wait |
| | `radv_wddm2_wsi_route.h`: the default route back to `gdi` |
| | `tests/radv_wddm2_wsi_route_test.c`: two more cases, the deadline rules and the b26 failure played through them |
| bc250-win, `wsi/b26-vk-dxgi` (`wsi/vk-dxgi-b23` and `wsi/vk-dxgi` are the same note on the earlier bases) | This note, `tools/build/build-radv-wsi-route-test.ps1` and `radv-wsi-route-test.py` |
| bc250-win, `wsi/b27-vk-dxgi` | This note's BD-105 sections, and `radv-wsi-route-test.py` hashing the second header into its record |

**32-bit processes.** The release carries two Vulkan ICDs: `payload/vulkan/vulkan_radeon.dll` from the system
line, which this branch changes, and `payload/wow64/vulkan/vulkan_radeon.dll`, which is the x86 build of the
D3D ICD line. One build serves the 32-bit D3D11 shell and 32-bit Vulkan, which
[the third-party list](../testing/THIRD-PARTY.md) records. A release that rebuilds the system line only
therefore leaves a 32-bit Vulkan application on the CPU path. Nothing in the code is 64-bit specific. Earlier
the system line could not build x86 at all. The b25 dispatch-table generator merge removed that, so this branch
builds an x86 candidate with the same five gates. Whether the wow64 Vulkan slot takes it is a decision of the
train cut, not of this note. The slot change needs its own 32-bit arm, and until that arm runs the release
notes must say that a 32-bit Vulkan application keeps the old path.

Three upstream defects are fixed on the way. The device-wide `wsi_device::blit` hook took the DXGI blit for a CPU
swapchain on a DXGI-capable device, and that blit reads a fence array which only a DXGI swapchain has. The present
of a CPU image waited for the GPU on a software device only, so the CPU could read an image the GPU had not
finished. Swapchain destroy also leaked each image's D3D12 resource, command list and allocator. The first two
defects are on the path of this route, because a swapchain of this route falls back to CPU images on a device that
keeps the DXGI hooks.

## Risks

- The second device's memory and creation time are not measured. A Vulkan game that recreates its swapchain
  often pays the creation once only, but the device stays for the life of the process.
- The linear application image costs render bandwidth compared with a tiled one. A tiled image with a
  Vulkan-side copy into the linear shared resource is the alternative if the lab measures the cost.
- The DXGI path of `wsi_common_win32.cpp` has two images (upstream limit). It reports `minImageCount` and
  `maxImageCount` as 2, and nothing clamps the count an application asks for. An application that needs three
  images cannot get them on this route, and `gdi` is its answer.
- `DwmFlush` completes `vkWaitForPresentKHR` at the next composition, which is a bound, not a measure of the
  flip that put the frame on the screen. A call with timeout 0 therefore returns `VK_TIMEOUT` for a present that
  DXGI has already queued, and an application that polls with timeout 0 alone never sees the present complete.
  Neither DXVK nor vkd3d-proton polls that way. A client that does needs the completion boundary to move.
- **The route offers three surface formats that the registered driver does not offer at all**, and no lab run has
  made a swapchain of one of them: `R8G8B8A8_SRGB`, `A2B10G10R10_UNORM_PACK32` and `R16G16B16A16_SFLOAT`. They are
  offered on the DXGI route only and have no CPU path, so such a swapchain cannot fall back. It fails with
  `VK_ERROR_INITIALIZATION_FAILED` when the DXGI route cannot make it. An application that picks the deepest format
  the surface reports therefore meets untested code. The switch for it is the route switch, not one of its own.
  Step 1 of the lab plan reads the offered list with the installed `vulkaninfo`, so the trial says which pairs the
  route offers. A client that asks for one of them is the test this work still needs.
- The route creates every chain with `DXGI_SCALING_STRETCH`, which upstream Mesa uses for its composition chain.
  A native D3D12 game creates a window chain with `DXGI_SCALING_NONE`. The source and the destination have the same
  size on this route, so stretch should not force composition, but no measurement says so on this hardware. It is
  the first thing to change if step 5 reads `COMPOSED` with nothing over the output.
- The route runs its copy on the D3D12 shell's queue, so the shell's hosted device and the application's RADV
  device share the GPU through two contexts. Their order is the shared fences only.
- The route is the default before any lab run. A defect that the fallbacks do not catch (a wrong picture, a
  hang in the D3D12 queue) reaches every pure Vulkan application until `gdi` is set.
- System32 `d3d12core.dll` delay-imports `dxgi.dll!CreateDXGIFactory2` (10.0.26100.9278). When an
  application-local DXVK `dxgi.dll` is already in the process, that import binds to DXVK, as the `d3d11` import
  does in [M792](../facts/d3d.md#m792). In M792 only the NULL-adapter path reached that import, and this route
  passes an explicit adapter. The DXVK step of the lab plan is the first measurement on unit A.
- A replacement D3D12 core is detected only after `D3D12CreateDevice` returns. If vkd3d-proton builds its device
  on this ICD during that call, the re-entry happens before the check.
- An application that wraps the D3D12 device with a module of a different name passes the implementation check,
  and the layout check (`check_blit_image`) is then the guard.

## Lab plan

**After BD-105 the plan has one decisive arm, and it is not a game.** vkcube with `dxgi` asked reproduces the
freeze, as it did in the b26 trial. It needs no game and it fits in 170 s, so it is the arm that says whether
the route presents at all. Three answers are all a result. The route presents, and the stage lines run to
`present1-returned`. Or the deadline catches the freeze, the last stage line names the call, and the client
falls back to CPU images inside the same arm. Or the freeze is in a call no deadline covers, and then the same
arm takes the user-mode stack of the frozen client with the portable `cdb` that is already on the lab. Only
after that arm presents does a game arm mean anything. The game arms run at **1280x720**, the resolution
of the released Quake II RTX numbers: the b26 gate compared a 1920x1200 run with 720p numbers and read a
halving that was 2.5 times the pixels. The 1920x1200 borderless run keeps its own baseline as the M15.14 arm.
Every ssh call of a step must use a timeout above the step's own bound. The b26 `dxgi` arm was lost to a 120 s
default against a 170 s step.

The kit that runs it is `scratch/b24/vk-wsi-dxgi-kit/lab/` (local, outside this repository): one PowerShell 5.1
script per step, `install-candidate.ps1` and `rollback-candidate.ps1` for the file swap, `EXPECTED.md` with the
expected line of every step, and `selftest.ps1` for the reading logic. Nothing of it has run. The kit takes the
candidate file and its hash as parameters, so the b26 candidate needs no new kit.

**The base of the plan is 0.7.216.100-tester.23 and the b26 candidate ICD.** The lab holds that release. Each
step uses it with one file replaced: `vulkan\vulkan_radeon.dll` under the install root, the system Vulkan ICD.
The manifest beside it names `.\vulkan_radeon.dll`, so that one file is the whole install, and the shells keep
their own ICD. The kit keeps the tester.23 file by hash and restores it by hash. The route's own rollback is
`AMDGPU_WDDM_VK_WSI=gdi` for one process, or the registry value `gdi` for all.

**Steps 1 to 6 are non-game arms of at most 170 s each, which holds the three-minute rule.** Step 7 is the one
game arm. It runs under the owner's game-session rules of 2026-10-01 and ends within 1200 s. The operator owns
the timer and ends the arm when its readings are in hand. No other step starts a game.

**Step 0, the control arm, comes first.** Run step 1 on the installed tester.23 file, with no replacement and no
variable. It must give the CPU present path: no `BC250 WSI: route=` line at all, no `D3D12 presenter device`
line, and the GDI present in ETW. This arm proves that each reading of steps 1 to 7 belongs to the candidate
bytes and not to the release. Record the hash of the installed file in the same record.

**The shipped fixes keep their own arms.** The candidate carries the Windows shader disk cache and the BD-102
BVH node address of tester.23. Step 7 is therefore a regression arm for the Q2RTX release gate as well as a
present arm: a hang or a wrong picture there fails the candidate even when every present reading passes.

Each client runs as a one-shot scheduled task of the interactive session. An SSH session on this lab has
elevation but lives in session 0, where a window is on no monitor and no interactive DWM composes it, so a present from
there could never be composed or flipped. Window work (borderless for step 5, the resize in step 3) happens in
that session too, because a window of session 1 cannot be moved from session 0.

Capture ETW with the present-mode provider set of `tools/win/lab-runner/etw/etw-capture.ps1 -PresentMode`, and
read it with `tools/win/etw/etw-present-mode.py`.

1. **vkcube, window, no variable and no registry value set, 60 s.** Collect `BC250_WSI_PRESENT_LOG`, the
   amdgpu-wddm log and ETW. Pass: the init line `BC250 WSI: route=dxgi asked=dxgi source=default ... reason=ok`,
   `D3D12 presenter device created ... impl=system:d3d12core.dll`, a `window swap chain` line, present log rows
   with path `dxgi` and `copy_us` 0, and no `CPU images` line. `etw-present-mode.py gpu.etl vkcube` reports
   present-history tokens with model REDIRECTED_FLIP and `Present` flags 0x9000 for vkcube, and no "no
   present-history token" presents. The KMD CPU blit counter does not advance. Also record the private bytes of
   vkcube before and after the first swapchain and the time of `vkCreateSwapchainKHR`. Before the client, record the
   surface format and present-mode lists with the installed `vulkaninfo`. The 8-bit pair must be there. The
   deep-colour pairs are evidence of what the route offers, and no step makes a swapchain of one.
2. **vkcube, window, `AMDGPU_WDDM_VK_WSI=gdi`, 30 s (rollback check).** Pass: `route=gdi asked=gdi source=env
   reason=asked`, present log rows with path `gdi`, no `D3D12 presenter device` line, and the ETW present of
   the GDI path. Then set the registry value `WsiRoute=gdi` with no variable for 20 s: `source=registry`, the
   same result. Remove the value at the end of the step.
3. **vkcube, window, present modes, 60 s.** Run FIFO, MAILBOX and IMMEDIATE for 20 s each. Pass: FIFO near the
   refresh rate, MAILBOX and IMMEDIATE above it with no `vkQueuePresentKHR` time near a refresh in the present
   log. A resize of the window gives new chains with `taken over from oldSwapchain` and no error.
4. **vkcube with an upstream DXVK `dxgi.dll` next to a copy of `vkcube.exe`, 60 s.** Pass: as step 1,
   and the init line has `app-local=dxgi.dll(file) bound=System32` (`loaded` if something in the process loaded
   it). The presenter line has `impl=system`. No vkcube window error and no `CPU images` line.
5. **vkcube, borderless 1920x1200, 70 s, with no experiment and the overlay hidden.** This step is the M15.14
   Vulkan criterion. Sessions 479 and 480 (2026-10-08) gave the native path an independent flip with no experiment at
   1920x1200: exclusive and borderless both read INDEPENDENT FLIP with the release defaults, so
   `AMDGPU_WDDM_D3D12_EXPERIMENT` stays empty. The overlay window must be hidden, because a topmost window
   over the output makes DWM compose (479/480 arm d). Pass: `etw-present-mode.py gpu.etl vkcube
   --admitted-address ... --kmd-counters ...` prints `M15.14 VERDICT vkcube ... result=INDEPENDENT-FLIP` and
   `M15.14 INCREMENT2 vkcube ... result=CONFIRMED`, the KMD scan-out flip and request counters both move, and
   every admission refusal column stays at zero. The front's `answer=1 rule=supported` line is evidence only
   when `RouteLogDirectory` was already set when DWM started, because the front reads that value at
   `OpenAdapter`. Its absence is not a failure of the step. A control arm with the overlay shown must read
   COMPOSED with no scan-out flip. A COMPOSED verdict with nothing over the output is the M15.14 result for
   native D3D12 as well, not a WSI defect.
6. **vkcube, `dxgi-composition`, 60 s.** Pass: as step 1, with the `composition swap chain` line, and present log
   rows with path `dxgi-composition`. The present log names the path from the chain's target, so the window route
   writes `dxgi` there and this route writes `dxgi-composition`. A `dxgi` row in this step is the finding.
7. **Quake II RTX 1.8.1, borderless at 1920x1200, no variable set. The one game arm, at most 1200 s.** It is
   the only Vulkan game the workspace stages for unit A (`scratch/pathtrace/pkg/q2rtx`, pushed to
   `C:\BC250\pathtrace` by `pt-session.py push`), and it is path-traced, so it exercises ray tracing and the
   WSI at once. The other games of the lab (The Witcher 3, Rise of the Tomb Raider, The Ascent, Factorio) have
   no Vulkan path, so none of them can test this route. The run is its own timedemo through
   `scratch/pathtrace/lab/pt-run.ps1 -Demo q2rtx-timedemo`, which already holds the interactive task, the Tctl
   gate and the thermal stop. Run the timedemo on the candidate, then the same timedemo with
   `AMDGPU_WDDM_VK_WSI=gdi` as the paired control, both inside the one session. Pass, in four parts: the
   readings of steps 1 and 5 for the game process, a frame rate of the candidate arm that is not below the
   `gdi` arm of the same session, no swapchain fallback line, and no TDR and no bugcheck in either arm. The
   release baseline for this game on
   tester.22 bytes is 59.4 frames per second for the pipeline API and 60.6 for the query API. A result below
   that baseline on `gdi` says the session, not the route, is the cause, so read the pair and not the absolute
   number. Q2RTX may also fail for a ray-tracing reason that has nothing to do with this WSI: the route lines
   appear before the game picks a device, so an early failure still names which route was chosen.

If a step fails on the DXGI route, the release notes keep `gdi` as the named workaround
(`docs/testing/release-notes/pending/vk-wsi-dxgi.md`), and the failure gets a GitHub issue. A failure of
step 7 blocks the wagon in any case: Quake II RTX is a release gate (owner, 2026-10-08).

### What runs offline before the lab

| Gate | What it proves |
|---|---|
| The five build gates of the system ICD recipe, on x64 and on x86 | The candidate has the shader disk cache, no live assert, the asked machine, no static `dxgi`/`d3d11`/`d3d12`/`d3d12core`/`dcomp` import, and the git sha of the branch head. The import gate is the one that keeps the route on System32 modules |
| `tools/build/build-radv-wsi-route-test.ps1` | The route rules: the switch and its three sources, the report of application-local modules, the D3D12 implementation check, and the LB7A import rules. Its negative control must fail every case |
| The pipeline stage-cover host test of this line | The ported tree still builds and passes the host test the shipped line carries |
| The fence-wait shape check and the BVH node address check | The port did not undo the two BD-102 fixes of tester.23. Each check reads its rule out of the tree first, and each one has a negative control |

The queue, sync and memory host tests belong to the D3D ICD line (`src/amd/vulkan/winsys/wddm2/tests/` on that
line). The system line has no source for them, so they cannot run on this candidate. The lab arms and the route
test take their place.
