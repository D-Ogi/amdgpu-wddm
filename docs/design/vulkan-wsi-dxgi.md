# Design: the Vulkan WSI presents through DXGI on a D3D12 device

Date: 2026-10-07. Ported to the b23 release line on 2026-10-08 and to the shipped b25 system ICD line on
2026-10-09. Status after the second lab trial of 2026-10-09: **the route has never presented a frame on unit A.
Its first present now runs to the end, and the client then stops in the acquire of the next frame, with the
GPU idle and no TDR (BD-105). The route is therefore opt-in and GDI is the default again.** GDI is also the
explicit rollback and the automatic fallback when the DXGI route fails. The owner's decision of 2026-10-07 (a route that must be switched on gets
forgotten) returns in the train whose lab arm presents a frame through this route.

What the two trials measured, and what the branch does about it, is the section
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
| What exists | The whole user-mode path is in upstream Mesa. A native D3D12 device on our adapter is the default for games (trials 053-056, M15.1). D3D12 shared resources and cross-API sharing work on unit A on the GPU route ([M802](../facts/d3d.md#m802)). That is prerequisite coverage. M802 measured a D3D12 producer with a D3D12 consumer and with a D3D11 consumer. It did not measure a RADV-exported timeline opened by D3D12, so the pair this route shares has its own cells ([the cross-stack share cells](#the-cross-stack-share-cells)). The D3D12 shell describes a shared resource as an LB7A linear surface ([D3D12 shared resources](../d3d12-shared-resources.md)), and RADV imports LB7A | Nothing. RADV writes no E26R record, and no public API wraps foreign allocations into an `IDXGISwapChain` | The shell presents only for a D3D12 device and a DXGI swap chain that the runtime created. Its present is a DDI behind the D3D12 runtime and dxgkrnl, not a call another module can make |
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
block was a CPU wait with no GPU work behind it.

The second trial of the same day named the call. With the deadlines of this section in the ICD, the first
present of the route ran to `present1-returned` for the first time in this defect's history, and the client
then stopped one frame later. The user-mode stack of the frozen process reads
`vk_common_WaitForFences` under `wsi_win32_acquire_next_image` under `wsi_AcquireNextImageKHR`, on the
application's message-pump thread, which is why the window stopped answering. That wait had no deadline,
because `route.presented` was set the moment `Present1` returned, and `Present1` only queues a present. The
answer to that is below, under "The route has presented when a present completed".

Whatever that call turns out to be, a wait of this route may not be unbounded. The rules are plain C in
`src/vulkan/wsi/wsi_win32_deadline.h` of the Mesa fork, with no Windows or Vulkan header, so the host test of
this lane drives the production rules:

- **One deadline, two seconds** (`WSI_WIN32_ROUTE_DEADLINE_NS`). This is a chosen bound, not a measured one.
  No present of this route has ever completed, so the cost of one is unknown: the b26 `dxgi` arm wrote no
  present row at all, and on that path `copy_us` is zero by construction. What is measured is the GDI path of
  the same file at 1920x1200, whose median present costs 1343 us of CPU copy plus 1832 us of BitBlt, that is
  **3.18 ms** over 640 rows, about 2.0 s of that 22.69 s timedemo. A GPU copy and a `Present1` have no reason
  to need three orders of magnitude more than that, and a lab trial has three minutes, so two seconds
  separates a slow frame from a dead route with room on both sides. Arm A2 of the lab plan is the first
  measurement of the route's own present cost. A present that needs more than this bound makes the bound
  wrong, and that arm says so.
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
  that instance takes CPU images with the reason `route-deadline`, and nothing re-arms the route inside the
  process. If the freeze of BD-105 sits in one of the two waits above, the application therefore gets a usable
  window back instead of a frozen thread. If it sits anywhere else, it is still a frozen thread: see what this
  does not cover, below. A late wait after one present has completed is a slow frame and changes nothing.
- **The D3D12 queue's own `Wait()` takes no deadline.** It is a GPU-side wait, and it is covered from the
  other end, because the acquire that waits for that copy is bounded now.
- **A first present logs the stage it enters**: `images` (the swapchain's images and their D3D12 blit
  contexts are ready), `queue-wait`, `execute`, `queue-signal`, `present1`, `present1-returned`, and then
  `acquire-wait` for the acquire of the next frame. One line each per swapchain, so a freeze names the call
  that did not return without a debugger on the machine. The b26 evidence stopped at the swap chain line,
  which left the image creation and the whole first present open as one window.

### The route has presented when a present completed

The second lab trial of 2026-10-09 measured what the flag above was worth. `route.presented` was set at the end
of `wsi_win32_queue_present_dxgi`, the moment `IDXGISwapChain3::Present1` returned, and
`wsi_win32_acquire_timeout_ns` hands the application's `UINT64_MAX` straight back once that flag is true.
`Present1` queues a present. Neither the GPU copy the route asked for before it nor the flip need have run.
So the one flag that disabled the route's only deadline was set by a call that proves nothing about
completion, and from the second frame on the acquire was unbounded again.

`presented` now means a present of this route **completed** and an acquire after it returned.

Completion is read from the image's **shared blit fence**. `wsi_dxgi_blit` signals
`d3d12_blit_fences[i]` to `base.blit.timeline_values[i]` after `ExecuteCommandLists` of the copy into the back
buffer, and that fence is the D3D12 side of the Vulkan timeline semaphore of the same image. So
`GetCompletedValue() >= that value` says, in one read of an object the chain already holds, that the presenter
queue's `Wait` for the application's own signal was satisfied, that the copy executed and that the queue
retired the `Signal`. That is the end of the route's own GPU work, and it is exactly what the trial says never
happens: the kernel driver's **presentation** counter `blits` stayed at 0 while both driver stacks waited.
That counter, with `scanout_requests` and `scanout_flips`, counts what passes the kernel driver's present
path: `Blits` is "presents copied", `ScanoutRequests` and `ScanoutFlips` are counted in
`SetVidPnSourceAddress` (`driver/kmd/wddm.c`, the counter block of the device record). They say nothing about
other submissions, so "no blit" is a statement about the presentation path and not about an idle GPU.

Two candidates were rejected from the code, not on taste. DXGI frame statistics
(`IDXGISwapChain::GetFrameStatistics`) describe the presentation engine and not our copy, are documented to
fail with `DXGI_ERROR_FRAME_STATISTICS_DISJOINT` on a first call, and report nothing useful for the
composition swap chains this file also makes. A fresh present fence signalled on the presenter queue after
`Present1` proves only that the queue drained past the copy, which the blit fence already proves, and
`wsi_win32_flush_d3d12_queue` names the per-frame cost of an object, an event and a wait.

Both halves of the rule are needed, and each covers the other's blind spot. The completed present says the
presenter's GPU work ran at all, which the return of `Present1` did not. The returned acquire says the very
wait that `presented` would disable can finish, which a fence value on its own does not. A route that a wait
already retired is never revived.

What the blit fence does not prove is that DWM or the screen scanned the frame out. The proof that would is
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` with `GetFrameLatencyWaitableObject`, and that is a
change at swap-chain creation with a buffer count to match. It belongs to the round after the route copies a
frame at all.

**And the expired acquire now writes the measurement the next trial needs**, so that no debugger has to:
`image <i> blit fence have <have> want <want>, queued present image <p> value <v>`. `want` is one past the
value the application's own queue signals, and three readings decide the cause. `have < want - 1`: the
application's own signal never reached the GPU, which is our submission path and not the presenter's.
`have == want - 1`: the application signalled, the presenter's `Wait` was satisfied, and the `Signal` after
the copy did not retire, which is the mutual-wait shape the frozen stack supports. `have >= want`: the copy
completed and the fence is done, and the Vulkan fence wait on our own side is the defect.

**What the deadlines do not cover.** Two waits are bounded: the queue flush and the first acquire. Everything
in `wsi_win32_image_init` has no deadline in front of it, which is `GetBuffer`, the shared blit resource
(`CreateCommittedResource`, `CreateSharedHandle`), the Vulkan import of that D3D12 resource, the layout check
and the command list. The b26 evidence cannot say which of the two windows the freeze is in, because nothing
in either logged, and that is why the stage lines exist. A freeze in the second window is outcome 3 of arm A2:
the last stage line names the call, the client is still frozen, and the arm takes a user-mode stack.

What this does not do: it does not make the route present. If the freeze is in a wait this change bounds, the
route now ends as a bad frame rate and a named stage instead of a dead game, which is the difference between a
lane experiment and a release defect. If it is not, the change has still named the window it is in.

### A dead route retires the work it queued

The third lab trial of 2026-10-09 ran the deadlines above and found the half of the answer they do not give.
The route retired itself after 2000 ms, as designed, and the client froze anyway. The expired acquire read
`image 0 blit fence have 18446744073709551615 want 2`, and `18446744073709551615` is `UINT64_MAX`, which is
what `ID3D12Fence::GetCompletedValue` answers for a device that has been **removed**. The user-mode stack of
the frozen client then named a call of its own: `vk_common_DeviceWaitIdle` under the recreate path the
`VK_ERROR_OUT_OF_DATE_KHR` had sent it into.

That freeze is a property of the present shape and not of the deadline. `wsi_common_queue_present` leaves
**two** submissions on the application's queue per presented image. The first signals the image's shared blit
timeline semaphore to `V`. The presenter's D3D12 queue then waits `V`, copies into the back buffer and signals
`V + 1`. The second submission waits `V + 1` and signals the image's own Vulkan fence and semaphores. Only the
presenter's `Signal` can reach `V + 1`, so a presenter that is gone leaves a queue submission waiting on a
value nothing in the process can produce. `vkDeviceWaitIdle` waits for that submission, and it takes no
timeout. The route's own deadlines cannot cover it: by then the route has already answered, correctly, that it
is out of date.

So a route that retires **signals the Vulkan side of every outstanding shared blit timeline from the CPU**, to
the value the presenter's `Signal` should have reached:

- `wsi_win32_route_retire_action` in `wsi_win32_deadline.h` is the rule, in plain C, so the host test drives
  it: an image with a recorded present value above the semaphore's current value is a candidate, and anything
  else is left alone. An image that never presented, and a timeline the presenter did reach, are both left
  alone, and a second pass over the same chain finds nothing to do. Round 4b made the rule answer one of
  three actions instead of a value, because a candidate is not yet a licence to signal (below).
- `wsi_win32_retire_blit_waits` applies it with `vkGetSemaphoreCounterValue` and `vkSignalSemaphore`, both
  core in Vulkan 1.2 and both new entries of the WSI dispatch table. It runs on the two expiry paths (the
  acquire and the queue flush) and on swapchain destroy of a route that is already retired, never on a live
  one. Each image it releases logs the two values and the result.
- A CPU signal exists for a timeline semaphore and for nothing else. There is no `vkSignalFence`, and none is
  needed: the released second submission signals the image's fence and binary semaphores itself, which is
  exactly the state the application is waiting for.
- Signalling a timeline from the CPU to a value the GPU was going to signal is safe **only** when the GPU will
  not signal it. Over-signalling a live timeline is a specification violation, so the live route is never
  touched, and round 4b replaced "the route retired" with a proof that the presenter is gone (below).

Two smaller changes come with it. A retired route stops offering the three surface formats that have no CPU
path, so the swapchain the client creates after the error falls back to CPU images instead of answering
`VK_ERROR_INITIALIZATION_FAILED`. And every expired wait logs `presenter device removed reason 0x<hr>` from
`ID3D12Device::GetDeviceRemovedReason` on the presenter device, which turns the `UINT64_MAX` reading from an
inference into an HRESULT.

### The presenter's own removal site

The removal is in our own D3D12 user-mode driver: the presenter line of round 3 reads
`impl=system:D3D12Core.dll` on our adapter, so the device the route holds is a device of
`amdgpu_wddm_d3d12.dll`. Round 3 could not name the site, because the shell's removal path wrote nothing a
release build keeps: `Device::remove` called `ddi_failure_note`, which needs `AMDGPU_WDDM_DDI_TRACE=2` and
writes to the debugger channel only.

The shell therefore logs the **first** removal of a process on the always-on channel, once, behind no switch:
`ddi_first_removal` in `ddi-trace.h` is a one-shot `std::atomic<bool>` exchange that writes
`device removed: <what> at <file>:<line>, the first removal in this process` through `ddi_refusal`, the
budgeted dual-channel path every refusal already uses. `Device::remove` takes `__builtin_LINE()` and
`__builtin_FILE()` defaults, so every one of its call sites names itself with no change at the call site, and
`HostedDispatch::remove_device` passes the site it was already given. One line per process bounds the cost: a
removal storm cannot flood the channel, and the first removal is the one that matters, because the later ones
are its consequences.

The candidate sites, read out of the tree, are the queue submission path (`native-queue-ddi.cpp`,
`native_execute`, where an engine `execute` other than `S_OK` removes the device), the deferred replay worker
whose removal is reported later through `report_deferred_removal`, and the admitted-create clamp. The replay
worker is the one that would explain round 3 exactly: a `Present1` that returned, and a removal reported after
it from another thread. The lab arm reads the ICD's HRESULT line first and the shell's site line in the round
that installs a shell, because the d3d12 slot is a hash-pinned registered triplet and not a file copy.

## Round 4b: the contract defects of round 4

Round 4 was reviewed and approved inside this work. An independent audit of exactly the same two revisions
then read the route call by call against the Vulkan specification and the D3D12 reference and found six
contract defects that the review had missed. None of them is a style question: each one is a rule of a
contract we do not own. They are listed here with the clause they break, because the next reader of this file
needs the clause and not the verdict.

**V1. A host signal must not pass a pending signal.** `vkSignalSemaphore` requires the new value to be
greater than the current value and **less than** the value of any pending signal operation on the same
semaphore (`chapters/synchronization.adoc`, VUID-VkSemaphoreSignalInfo-value-03258 and -03259). Round 4
signalled the blit timeline to `V` whenever the route had retired. But `V` is the value the application's
**first** submission signals, and that submission is still pending whenever the freeze is in the first window
rather than the second. A 2000 ms timeout proves neither that the work completed nor that the presenter is
dead, so round 4 could break the VUID on a route that was merely slow.

Round 4b separates three states and acts only in one.
`wsi_win32_presenter_state` answers `REMOVED` on a proof (`GetDeviceRemovedReason` other than `S_OK`, or a
blit fence whose completed value is `UINT64_MAX`), `LIVE` on a reading that contradicts removal, and
`UNPROVEN` when neither read succeeded. The retire path signals only when the presenter is **proved removed**
and the preceding Vulkan signal of `V` has completed on our own device, which it establishes with a bounded
`vkWaitSemaphores` on `V` - one below the presenter's `V + 1` - inside
`WSI_WIN32_ROUTE_RETIRE_DEADLINE_NS` (200 ms) and a re-read. When that
wait expires, or the presenter is not proved removed, the route **refuses** and logs the refusal with the
VUID. It does not manufacture a completion. The caller then reports `VK_ERROR_DEVICE_LOST` when work is still
outstanding and `VK_ERROR_OUT_OF_DATE_KHR` when it is not, which `wsi_win32_route_report` decides. Both are
answers the WSI contract allows for `vkAcquireNextImageKHR`, and `OUT_OF_DATE` is the one that lets the
client fall back to the CPU route, so a proved-dead presenter whose queue could be retired safely still ends
as a recreated swapchain and not as a lost device.

**V1 again: whose value is it?** The review of round 4b found the VUID still reachable, through round 4b's own
change. `blit.timeline_values[i]` is written by two signallers: `wsi_common_queue_present` pre-increments it to
`V` for the application's own first submission, and `wsi_dxgi_blit` raises it to `V + 1` only after an accepted
`ID3D12CommandQueue::Signal` - which is the V2 fix above, and which is what leaves the application's own pending
value in the entry on every failure return of the blit. The rule proved its signal with
`semaphore == present value - 1`, and in that state the equality means the application's signal of `V` is
**pending** and the semaphore reads `V - 1`: the one state 03259 forbids passing. The measured shape, on the
two-image chain of lab round 3: present image 0, the submission signalling `V = 1` still pending so the
semaphore reads 0, the blit fails and returns `VK_ERROR_DEVICE_LOST`, `timeline_values[0]` keeps 1, the second
submission and the present are skipped. A later acquire expires, the route retires, the presenter reads
`REMOVED` - and the retirement would signal 1 with 1 pending. The same after one good cycle with `V = 3` and
the semaphore at 2, and the chain's teardown reaches it too.

So the rule asks a second question that no arithmetic on those values can answer: **did the presenter's Signal
of the value this image carries get accepted?** `struct wsi_win32_image_debt` is one atomic word per image,
cleared when `wsi_dxgi_blit` is entered for that image and set after the accepted `Signal` has raised the value,
in that order, so a reader that sees the debt always sees the presenter's value with it. The clear loses
nothing: entering the blit for an image means the application acquired it, and that acquire waited for the
fence the previous cycle's second submission signals. An image the presenter owes nothing for has nothing of
**ours** outstanding behind it, because a blit that does not return `VK_SUCCESS` skips the second submission
and the present, so the only submission left is the application's own, waiting on the application's own
semaphores on a device that is alive. The retirement answers `NOTHING` there, says so in one line per image,
and the acquire reports `VK_ERROR_OUT_OF_DATE_KHR`: reporting it as outstanding would end a client with
`VK_ERROR_DEVICE_LOST` over a wait that does not exist.

**V1 a third time: which value?** The second review of the same round found that fact aliasing one level up.
The debt was a **bit** - "the presenter owes one more value" - and a bit names no value, while the entry it is
read against moves without it: `wsi_common_queue_present` pre-increments `blit.timeline_values[i]` to `W + 1`
for the application's next submission **before** that submission is made, and the clear of the debt lives
inside `wsi_dxgi_blit`, which that path skips when the submission fails. After any `vkQueuePresentKHR` whose
internal submit failed, the entry therefore holds `W + 1` while the bit still records the accepted `Signal` of
`W`, and a retirement with the presenter proved removed read `want = W + 1`, `have = W`, `owes = true` and
host-signalled `W + 1`: a value nobody promised, and, when the application's own signal of `W + 1` is pending
on a lost queue, 03259 again. The same window is open to any concurrent retirement, and the record is read
across threads by construction, so no argument about which thread calls what closes it.

The record now carries the owed **value** next to the word, stored before it, and the question is `owes =
word != 0 && value == entry`. Every stale or aliased reading then collapses to `NOTHING` by construction: a
newer cycle's word with an older cycle's value fails the comparison, and a newer value with no word yet fails
the word. `NOTHING` is the direction a mistake has to fall, because it costs the client an
`VK_ERROR_OUT_OF_DATE_KHR` it can recreate from, while an unproved host signal is the defect being fixed. The
64-bit half of the pair is spelled with `_InterlockedCompareExchange64` alone, because `_InterlockedOr64` and
`_InterlockedExchange64` are x64/ARM intrinsics while the 64-bit compare-exchange is documented for x86 too,
and the release ships an x86 ICD beside the x64 one.

**One unbounded wait of ours is left on the route.** `wsi_common.c`'s throttle inside `vkQueuePresentKHR`
waits on an image's own `VkFence` with `WaitForFences(..., ~0ull)` before it reuses that image. In practice the
bounded acquire is always reached first - a chain of `N` images gives `N` fast-path acquires before any image
is presented twice - so BD-105 cannot move into that call. It is named here because it is the one wait on this
path that no deadline of ours covers, and because a future change to the image count or to the acquire's
fast path is what would make it reachable.

**V2. The HRESULTs of the blit path were dropped.** `ID3D12GraphicsCommandList::Close`,
`ID3D12CommandQueue::Wait` and `::Signal` all return an HRESULT that the caller is expected to read
(`sdk-api-docs`, `nf-d3d12-id3d12graphicscommandlist-close.md` and the queue pages), and
`ExecuteCommandLists` returns `void`, so its failures appear only as a removed device. Round 4 ignored all of
them and returned `VK_SUCCESS` from the blit regardless. A route that cannot close its list then presented an
image it had not copied, and `wsi_common_queue_present` went on to its second submission and its present:
exactly the shape of a freeze with nothing in the log.

Round 4b checks every one of them, and the first failure of the route is kept in one place.
`wsi_win32_route_note_error` claims the record with a compare-and-swap, so exactly one caller logs
`FIRST route failure: <call> hr=0x... image <n>, presenter removed reason 0x...`, always on, once per route.
`ExecuteCommandLists` is followed by a read of the removed reason, which is the only failure channel a `void`
call has. A failure stops the dependent work instead of queueing more of it: a refused `Close` fails the blit,
a refused `Signal` leaves `base.blit.timeline_values[i]` **unraised**, so the value the retire rule compares
against is still the truth.

**V3. Route flags were plain `bool`s shared between threads.** A non-atomic object read by one thread while
another writes it has no defined value (`llvm/docs/Atomics.rst`, the section on data races). The two flags
were written on the present path and read on the acquire path of other swapchains. Round 4b makes the route
state one `uint32_t` of bits with compare-and-swap accessors. The header is compiled as C11 by `cl /TC` and
as C++ by the Mesa MSVC build, so the shims are `_InterlockedOr` and `_InterlockedCompareExchange` on MSVC
and `__atomic_load_n` and `__atomic_compare_exchange_n` elsewhere, not `<stdatomic.h>`. Each transition is
claimed by exactly one caller, which is what makes "log this once" true rather than likely. The host test
drives eight threads over two thousand rounds on distinct swapchains and counts the claims.

**V5. A drain timeout released resources the GPU might still read.** D3D12 requires a resource to stay alive
until the GPU has finished referencing it (`direct3d12/binding-model.md`: resources are kept alive by the
application, not by the runtime). `wsi_win32_flush_d3d12_queue` returned `void`, and its callers released or
reused the chain's resources whether it had drained or not: on a fence-creation refusal, on a failed
`Signal`, and on the expiry of its own wait. Round 4b has it answer `DRAINED`, `REMOVED` or `UNPROVEN`.
`DRAINED` and `REMOVED` release. `UNPROVEN` pins the whole dependent set (the imported memory, the source and
destination resources, the allocator and the drain fence) by setting `resources_pinned` on the chain and
returning without freeing anything, and a later `vkCreateSwapchainKHR` for the same window answers
`VK_ERROR_INITIALIZATION_FAILED` rather than reusing a chain whose buffers may be in flight. A leak that the
process gives back at exit is the right trade against a use-after-free in a shared surface. The command
**list** is not in that set: destroying that interface before its prior executions have completed is
explicitly allowed (`DirectX-Specs`, `d3d/CPUEfficiency.md`).

**V6. The NT handle of our own shared resource was never closed.** `VkImportMemoryWin32HandleInfoKHR` does
not transfer ownership of the handle: the application keeps it and must close it
(`chapters/memory.adoc`, the import-ownership paragraph). The route created the handle with
`CreateSharedHandle` and leaked it on both paths. Round 4b closes it after the import attempt, on success and
on failure, and the host test drives both outcomes against a model of the handle table.

### The cross-stack share cells

The route shares two kinds of object, and until round 4b nothing had measured either of them in the
direction the route uses: M802 is a D3D12 producer with a D3D12 or D3D11 consumer, which is prerequisite
coverage of the kernel driver's sharing, not of a RADV timeline opened by D3D12. Two clients now measure the
pair, `tools/win/wsi-dxgi/sharecell12.cpp` and `tools/win/wsi-dxgi/sharecellvk.cpp`, over exactly the handle
types the route uses: a D3D12 committed texture created with `D3D12_HEAP_FLAG_SHARED` and imported as
`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`, and a RADV timeline semaphore exported as
`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT` and opened by `ID3D12Device::OpenSharedHandle`.

Each round writes a pattern whose every texel is a function of its coordinates and of the round, so a stride
misread appears as a diagonal, a channel-order error appears in one byte of four, and a stale image appears as the
wrong round constant. The fence schedule gives the producer the odd values and the consumer the even ones, so
a value that arrives on the wrong side cannot be explained away. The consumer waits on its own **queue**, not
on the CPU, because a CPU wait would not measure what the route depends on. Every CPU wait is bounded and
never `INFINITE`. The same image, allocation and timeline serve every round, which is the image reuse the
route does per frame. `sharecell12` is the D3D12 producer with the RADV consumer. `sharecellvk` is the
direction the route itself needs, RADV writing and the D3D12 queue reading.

Both carry a host mode (`--selftest`) that needs no device of either stack and drives the pure rules with a
negative control, and both stop at the first refusal with the name of the call that refused. They are the
first arms of the lab plan: a route that presents nothing because this pair does not work would otherwise be
debugged at the swap chain.

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
| Mesa fork, `amdgpu-wddm/b27-vk-wsi-dxgi`, which is `amdgpu-wddm/b26-vk-wsi-dxgi` plus the answer to BD-105 | `wsi_win32_deadline.h`: the deadlines, the completion rule, the retired route and the stage names, as plain C the host test drives |
| | `wsi_common_win32.cpp`: the bounded queue flush, the capped acquire, the stage lines, the refusal of the route after an expired wait. Two blit-fence readings: the one that lifts the deadlines, and the one an expiry logs |
| | `radv_wddm2_wsi_route.h`: the default route back to `gdi` |
| | `tests/radv_wddm2_wsi_route_test.c`: four more cases, the deadline rules, the completion rule, the stage bits, and the b26 failure played through them |
| | `9974c121` after the offline review. The two-second bound says what it rests on. The GDI path's 3.18 ms is the only measured present. A retired route promises a window only for the waits it covers. The fence of an expired queue wait stays with the queue, which still has its `Signal` outstanding. `wsi_win32_route_wait_expired` is read into a local, not into a log argument |
| | `8f0bfdfb` after round 2 of the review. The route test's own deadline comment had kept the present cost that was never measured |
| | `b433f564`, `281fbbcb` and `0c0ffa4c` after the second lab trial. `route.presented` follows a completed present and an acquire that returned. The seventh stage name is `acquire-wait`, and the stage log is gated per chain. The present log flushes a row of an unproved route at once. An expired acquire writes the blit-fence reading. The fence that proves the route is the acquired image's own. The chain's last present belongs to another image and lags by one frame for ever |
| | `cfa220dd` after the third lab trial. A retired route releases the submissions it left waiting on the shared blit timeline: `wsi_win32_route_retire_value` is the rule and `wsi_win32_retire_blit_waits` applies it. Both expiry paths log the presenter's `GetDeviceRemovedReason`. A retired route stops offering the formats with no CPU path. `wsi_common.c` takes `vkSignalSemaphore` and `vkGetSemaphoreCounterValue` into the WSI dispatch table. The route test takes the round-4 case |
| | Round 4b, after the independent audit of `cfa220dd` ([the section above](#round-4b-the-contract-defects-of-round-4)). V1: `wsi_win32_presenter_state` and `wsi_win32_route_retire_action` replace the value rule, and a host signal needs a proved-removed presenter and a completed `V - 1`. V2: every HRESULT of the blit path is read, and the route's first failure is claimed once and logged always-on. V3: the route flags become one word with compare-and-swap accessors. V5: `wsi_win32_flush_d3d12_queue` answers drained, removed or unproven, and an unproven drain pins the chain's resources. V6: the resource handle of the import is closed on both outcomes. The route test grows to seventeen cases, among them the refcount and handle-table models and the concurrent flag claims |
| bc250-win, `wsi/b26-vk-dxgi` (`wsi/vk-dxgi-b23` and `wsi/vk-dxgi` are the same note on the earlier bases) | This note, `tools/build/build-radv-wsi-route-test.ps1` and `radv-wsi-route-test.py` |
| bc250-win, `wsi/b27-vk-dxgi` | This note's BD-105 sections, and `radv-wsi-route-test.py` hashing the second header into its record |
| | `driver/umd/d3d12`: the first device removal of a process names its own site on the always-on channel (`ddi_first_removal`, one shot). `Device::remove` takes `__builtin_FILE()` and `__builtin_LINE()` defaults, so each call site names itself |
| | Round 4b, `driver/umd/d3d12`: `ddi_first_failure` keeps the first failing DDI of the process and of each group of the presenter's path. The groups are the list close, the queue synchronisation, the fence, the shared-resource open and the present. The ledger is always on and writes at most six lines in a process. `EntryPolicy::leave` and `::fast_denied` feed it. A refusal therefore names its call, its HRESULT and whether the device was lost by then |
| | Round 4b, `tools/win/wsi-dxgi`: `sharecell12.cpp`, `sharecellvk.cpp` and `sharecell-common.h`, the two cross-stack share cells and their host mode |

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
- The route is opt-in since BD-105, so a defect that the fallbacks do not catch (a wrong picture, a hang in a
  call no deadline covers) reaches only a process that asked for the route. It was the default for the whole of
  train b26, which is how one such defect reached every pure Vulkan client of that candidate.
- System32 `d3d12core.dll` delay-imports `dxgi.dll!CreateDXGIFactory2` (10.0.26100.9278). When an
  application-local DXVK `dxgi.dll` is already in the process, that import binds to DXVK, as the `d3d11` import
  does in [M792](../facts/d3d.md#m792). In M792 only the NULL-adapter path reached that import, and this route
  passes an explicit adapter. The DXVK step of the lab plan is the first measurement on unit A.
- A replacement D3D12 core is detected only after `D3D12CreateDevice` returns. If vkd3d-proton builds its device
  on this ICD during that call, the re-entry happens before the check.
- An application that wraps the D3D12 device with a module of a different name passes the implementation check,
  and the layout check (`check_blit_image`) is then the guard.

## Lab plan

**Round 4 of BD-105 adds one measurement and no arm.** The decisive arm is still vkcube with `dxgi` asked, and
its three outcomes are unchanged. What round 4 reads on top is the presenter's removal HRESULT in the route
log, the release lines of the retired blit timelines, and the client's **window** from a one-shot task of the
interactive session, because a client whose window answers is the whole of outcome 2 and round 3 measured only
half of it. An SSH session on this lab is session 0, where `EnumWindows` sees no window of the logged-in
desktop at all, so the probe has to run where the window lives. The verdict of the dxgi arms now gates on the
route's proof line, which is how a round that did not present can no longer print PASS.

**After BD-105 the plan has one decisive arm, and it is not a game.** vkcube with `dxgi` asked reproduces the
freeze, as it did in the b26 trial. It needs no game and it fits in 170 s, so it is the arm that says whether
the route presents at all. Three answers are all a result. The route presents, and the stage lines run to
`present1-returned`. Or the deadline catches the freeze, the last stage line names the call, and the client
falls back to CPU images inside the same arm, with the blit-fence reading in the log. Or the freeze is in a
call no deadline covers, and then the same arm takes the user-mode stack of the frozen client with the
portable `cdb` that is already on the lab. Only
after that arm presents does a game arm mean anything. The game arms run at **1280x720**, the resolution
of the released Quake II RTX numbers: the b26 gate compared a 1920x1200 run with 720p numbers and read a
halving that was 2.5 times the pixels. The 1920x1200 borderless run keeps its own baseline as the M15.14 arm.
Every ssh call of a step must use a timeout above the step's own bound. The b26 `dxgi` arm was lost to a 120 s
default against a 170 s step.

The kit that runs it is `scratch/b24/vk-wsi-dxgi-kit/lab/` (local, outside this repository): one PowerShell 5.1
script per step, `install-candidate.ps1` and `rollback-candidate.ps1` for the file swap, `EXPECTED.md` with the
expected line of every step, and `selftest.ps1` for the reading logic. Nothing of it has run on this candidate.
Each step says which route it asks for and how, because the default is GDI now: a step that expected
`route=dxgi asked=dxgi source=default` would report a failure on a driver that behaves exactly as designed,
which is the same class of defect as the variable collision that lost the b26 comparison arm. The verdict of a
step is a function the self test replays on fake logs, with a passing and a failing case per arm, and the swap
scripts take both hashes as parameters.

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
| The source gates of the round, each one with a control revision | Every rule this note describes is in the tree, and none of them passes on the revision before it. Forty-three of them after round 4b, nineteen of which were added by it and are controlled against `cfa220dd` |
| The pre-fix controls | The round's own host-test cases refuse to compile or fail against the headers of the rounds before them. Four after round 4, the last one the retire rule against `0c0ffa4c` |
| The D3D12 shell's experiment test | The one-shot first-removal line exists, spends exactly one refusal of the budget, and spends none on the removals after it. Round 4b adds the per-group first-failure ledger. Its cases are the group of every boundary name of the route's path and the refusal rule. It also costs the budget per outcome. The outcomes are a success, an `E_PENDING`, the first failure, a second failure of one group, a failure of another |
| The host mode of the two share cells, with its negative control | The pattern, the comparison and the odd/even fence schedule of the cross-stack cells are right before they reach a GPU. Eleven cases pass, and all eleven fail inverted |

The queue, sync and memory host tests belong to the D3D ICD line (`src/amd/vulkan/winsys/wddm2/tests/` on that
line). The system line has no source for them, so they cannot run on this candidate. The lab arms and the route
test take their place.
