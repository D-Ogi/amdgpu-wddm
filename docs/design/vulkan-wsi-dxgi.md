# Design: the Vulkan WSI presents through DXGI on a D3D12 device

Date: 2026-10-07. Status: implemented offline, not measured on unit A. The default route stays GDI until the lab
plan below passes. This note follows [ADR 0018](../adr/0018-engine-present-for-vulkan-wsi.md) and replaces the
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

1. At swapchain creation the WSI asks the driver whether the route may run (`route_allowed`). Then it creates
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

- **Recursion.** `vk_dxgi_create_d3d12_device(LUID)` loads System32 `d3d12.dll` by full path. The runtime loads
  our D3D12 shell. The shell loads its engine and its hosted RADV (`amdgpu_wddm_radv.dll`) with `LoadLibraryExW`
  by full path (`driver/umd/d3d12/adapter-caps.cpp`), not through the Vulkan loader, so the call never comes
  back into `vulkan_radeon.dll`. The hosted RADV runs with the host dispatch set, and the winsys keeps the route
  off there, so the shell's own instance never makes a D3D12 device either.
- **When.** The device is made at the first swapchain of a process, once per winsys, under a lock. A failure is
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
10-bit and FP16 primaries is a matter for the kernel driver and the shell's scan-out rules, not for the WSI.

## The switch and the fallback

- `AMDGPU_WDDM_VK_WSI` in the environment, else the REG_SZ value `WsiRoute` under
  `HKLM\SOFTWARE\amdgpu-wddm\Vulkan`: `gdi`, `dxgi` (window swap chain) or `dxgi-composition`. An empty
  environment value counts as absent. Any other value selects GDI and the log names it invalid.
- The default is GDI (`RADV_WDDM2_WSI_ROUTE_DEFAULT`) until the lab plan passes. Then one line changes the
  default to `dxgi`, and `gdi` stays the fallback switch, as ADR 0018 point 2 asks.
- **The module gate.** The route stands down when the process holds a `dxgi.dll`, `d3d12.dll` or
  `d3d12core.dll` from outside System32. An app-local DXVK `dxgi.dll` shadows the System32 one for System32
  modules ([M792](../facts/d3d.md#m792), [M794](../facts/d3d.md#m794)), and E56 named force-loading System32
  `d3d12` in such a process as a thing the WSI must not do ([M793](../facts/d3d.md#m793)). The gate reads every
  loaded module at instance creation and again at each swapchain, because two modules of one name can be loaded
  at once. The native D3D11 and D3D12 games do not use this WSI at all: they present through DXGI and our
  shells.
- **Per swapchain.** If the gate fails, the D3D12 device or queue cannot be made, or any DXGI step fails, that
  swapchain takes CPU images and the log says why. A format that has no CPU path then fails with
  `VK_ERROR_INITIALIZATION_FAILED`.

## Code

| Repository, branch | Change |
|---|---|
| Mesa fork, `amdgpu-wddm/vk-wsi-dxgi` | `radv_wddm2_wsi_route.h` (switch, gate, LB7A rules, host test), `radv_wddm2_bo.c` (10-bit and FP16 import), `radv_wddm2_wsi.c` (hooks, device, layout check), `wsi_common.c` (per-swapchain blit hook and CPU wait), `wsi_common_win32.cpp` (the route) |
| bc250-win, `wsi/vk-dxgi` | This note, `tools/build/build-radv-wsi-route-test.ps1` and `radv-wsi-route-test.py` |

Two upstream defects are fixed on the way. The device-wide `wsi_device::blit` hook skipped the own blit of a CPU
swapchain on a DXGI-capable device. The CPU-image present waited for the GPU only on software devices. Swapchain
destroy also leaked each image's D3D12 resource, command list and allocator.

## Risks

- The second device's memory and creation time are not measured. A Vulkan game that recreates its swapchain
  often pays the creation once only, but the device stays for the life of the process.
- The linear application image costs render bandwidth compared with a tiled one. A tiled image with a
  Vulkan-side copy into the linear shared resource is the alternative if the lab measures the cost.
- The DXGI path of `wsi_common_win32.cpp` has two images (upstream limit). An application that asks for three
  gets two.
- `DwmFlush` completes `vkWaitForPresentKHR` at the next composition, which is a bound, not a measure of the
  flip that put the frame on the screen.
- The route runs its copy on the D3D12 shell's queue, so the shell's hosted device and the application's RADV
  device share the GPU through two contexts. Their order is the shared fences only.
- An application that ships the Agility SDK (`d3d12core.dll` beside it) fails the gate and keeps GDI.

## Lab plan

Each step is one trial of at most three minutes. The Vulkan game step is a game session under the game bound.
Each step uses the installed release with only `vulkan_radeon.dll` replaced by the candidate, and the GDI
switch (`AMDGPU_WDDM_VK_WSI=gdi`) is the rollback. Capture ETW with the present-mode provider set of
`tools/win/lab-runner/etw/etw-capture.ps1 -PresentMode`, and read it with `tools/win/etw/etw-present-mode.py`.

1. **vkcube, window, `AMDGPU_WDDM_VK_WSI=dxgi`, 60 s.** Collect `BC250_WSI_PRESENT_LOG`, the amdgpu-wddm log and
   ETW. Pass: the log line `BC250 WSI: route=dxgi`, a `window swap chain` line, present log rows with path
   `dxgi` and `copy_us` 0, and no `CPU images` line. `etw-present-mode.py gpu.etl vkcube` reports present-history
   tokens with model REDIRECTED_FLIP and `Present` flags 0x9000 for vkcube, and no "no present-history token"
   presents. The KMD CPU blit counter does not advance. Also record the private bytes of vkcube before and
   after the first swapchain and the time of `vkCreateSwapchainKHR`.
2. **vkcube, window, present modes, 60 s.** Run FIFO, MAILBOX and IMMEDIATE for 20 s each. Pass: FIFO near the
   refresh rate, MAILBOX and IMMEDIATE above it with no `vkQueuePresentKHR` time near a refresh in the present
   log. A resize of the window gives new chains with `taken over from oldSwapchain` and no error.
3. **vkcube, fullscreen 1920x1200, 60 s, with `AMDGPU_WDDM_D3D12_EXPERIMENT=scanout-flip-1920x1200`.** Pass:
   the router front logs `answer=1 rule=supported`, and `etw-present-mode.py gpu.etl vkcube --admitted-address
   ... --kmd-counters ...` prints the `M15.14 INCREMENT2 vkcube` line with its pass result (all four clauses PASS). A COMPOSED verdict here is
   the M15.14 result for native D3D12 as well, not a WSI defect.
4. **vkcube, `dxgi-composition`, 60 s.** Pass: as step 1, with the `composition swap chain` line.
5. **A Vulkan game, borderless at 1920x1200, `AMDGPU_WDDM_VK_WSI=dxgi`.** Pass: as steps 1 and 3 for the game
   process, the game's own frame rate against the same session on `gdi`, and no swapchain fallback line.

After step 5 passes, the default changes to `dxgi` in one commit, and the release notes name the `gdi` switch.
