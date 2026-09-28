# Windowed present of a device-local image under DWM composition (research, 2026-09-27)

Read-only research for the E45 result (facts M598): a token-less `D3DKMTPresent(Blt, hWindow, hSource)` from a
non-UMD context returns `0xC01E0342 STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE` and `DxgkDdiPresent` never runs.

**Correction, 2026-09-27.** The CDD-DWM interop inference in this document is refuted by measurement.
Sections 1, 2d and 2e and the plan in section 4 read `DriverSupportsCddDwmInterop` as enough to put a window's
redirection surface on the GPU and admit the Blt. KMD 164 advertises the cap. Under a GPU DWM the same present
still fails with `0xC01E0342` (facts M677). The redirection handshake answers `BLT_VIA_GDI` (`0x263008`), and no
new GDI surface appears for the probe window (M682). The affected lines below are marked [EV] or "refuted";
the rest stands as written.

Evidence labels:

- **[DOC]** quoted from Microsoft documentation or headers in `ref\`.
- **[SRC]** quoted from open-source code or public review threads in `ref\`.
- **[INF]** inference from the documents. It is not stated in any of them.
- **[KNOW]** the author's background knowledge, not backed by a local file. Treat it as a hypothesis to measure.
- **[EV]** a later measurement on unit A, cited by its `docs/facts.md` row. Added after the first version.

Sources used, with revisions:

- `ref\ddi-display` (WDK/SDK 10.0.26100).
- `ref\windows-driver-docs\windows-driver-docs-pr` @ `110f60e`.
- `ref\win32-docs` @ `e103fa4`.
- `ref\mesa` @ `05e6c962`, dated 2026-09-25.
- `ref\presentmon-etw` @ `fe01b642`.
- `ref\web-docs\mesa-d3d10umd-zink-history\mr-24223.md`.

Short paths below are relative to `P:\bc-250\ref\` unless they start with `bc250-win\`.

## 1. How a windowed `D3DKMTPresent` Blt is routed under DWM, and where `VIDPN_SOURCE_IN_USE` comes from

**What the documentation says.**

- [DOC] `ddi-display\d3dkmthk.md:15074-15097`. The only documented ICD example sets exactly what E45 set:
  `Blt`, `DstRectValid`, `SrcRectValid`, `hWindow`, the rects, `SubRectCnt = 1` and `pSrcSubRects`. It sets no
  token and no `hDestination`. Note that it fills `PresentData.hDevice = hDevice` (`:15084`). `hDevice` and
  `hContext` share a union (`:28926-28930`), so the E45 context handle is equivalent.
- [DOC] `d3dkmthk.md:29001`: `hSource` is "a kernel-mode handle to the system memory or primary allocation to
  present from". The text names system memory or primary allocations only. A device-local non-primary source is
  not described. [INF] The limit comes from the pre-DWM model. It is probably not why E45 was rejected.
- [DOC] `d3dkmthk.md:29009`: "If the handle in the hDestination member is nonzero, the hDestination and hWindow
  handles must refer to two different primary allocations of the same size, the device in the hDevice member must
  own the video present source that is identified by the VidPnSourceId member". The same rule appears at `:15043`:
  "The source and destination are not a primary surface" is an `INVALID_PARAMETER` case. So `hDestination` can
  never name a DWM redirection surface. It exists for primary-to-primary blts by a VidPn source owner.
- [DOC] `d3dkmthk.md:29791` (`D3DKMT_PRESENTFLAGS.Blt`): "specifies whether to bit-block transfer (bitblt)
  data to the primary surface".
- [DOC] `d3dkmddi.md:9615`: `DxgkDdiPresent` "copies content from source allocations to a primary surface (and
  sometimes to off-screen system memory allocations)". The destination is "either the current primary of the
  device or an off-screen system memory allocation" (`d3dkmddi.md` Remarks, same section).
- [DOC] `STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE` is documented only for `D3DKMTSetVidPnSourceOwner`
  (`d3dkmthk.md:16772`) and `D3DKMTCheckVidPnExclusiveOwnership` (`:1788`). Both say the source "is already owned
  by a display mode manager (DMM) client and cannot be used until the client releases the video present source".
  No document lists it as a `D3DKMTPresent` return code (`:15016-15025`).

**What open-source evidence says.**

- [SRC] `presentmon-etw\PresentData\PresentMonTraceConsumer.cpp:440-470`. dxgkrnl emits a `Blit_Info` event
  carrying `hwnd` and `bRedirectedPresent` for every windowed blt present. PresentMon classifies the event two ways:
  - `bRedirectedPresent = 1` becomes `Composed_Copy_CPU_GDI`.
  - `bRedirectedPresent = 0` becomes `Hardware_Legacy_Copy_To_Front_Buffer` at first.
- [SRC] `:809-814`. When a present history token of model `D3DKMT_PM_REDIRECTED_BLT` (or uninitialized) follows
  a non-redirected blt, the mode is upgraded to `Composed_Copy_GPU_GDI`.
- [SRC] `presentmon-etw\README-ConsoleApplication.md:140-145` defines the modes:
  - "Hardware: Legacy Copy to front buffer": "the app took ownership of the screen, and is copying new contents to
    an already-on-screen surface".
  - "Composed: Copy with GPU GDI": "the app is windowed, and is copying contents into a surface that's shared with
    GDI".
  - "Composed: Copy with CPU GDI": "copying contents into a dedicated DirectX window surface".
- [SRC] `web-docs\mesa-d3d10umd-zink-history\mr-24223.md:86`, from jenatali (Microsoft, D3D/GL team): "`DrvPresentBuffers`
  should call `D3DKMTPresent` instead of `D3DKMTRender`, at which point, the KMD will be asked to record a blit.
  This blit targets the GDI redirection bitmap, and respects clipping rects".

**Routing, reconstructed.** [INF] from the PresentMon sources and the jenatali comment.

1. dxgkrnl looks up `hWindow`.
2. If the window is DWM-redirected and its GDI redirection surface is a GPU allocation, dxgkrnl names that surface
   as the destination of `DxgkDdiPresent`. It clips with the window region and then queues a `REDIRECTED_BLT`
   present history token so DWM recomposes. PresentMon shows this as "Composed: Copy with GPU GDI". This is the
   documented OpenGL ICD example working on Windows 8 and later.
3. Otherwise the blt is a legacy copy to the front buffer ("the app took ownership of the screen"). That needs
   access to the VidPn source's primary.

**Why E45 got `VIDPN_SOURCE_IN_USE`.** [INF] The DWM holds the VidPn source. The two ownership calls return
exactly this status when a DMM client owns the source. So the likeliest reading of E45 has three steps:

- dxgkrnl found no GPU redirection surface for the vkcube window.
- It therefore fell to the legacy front-buffer path.
- It refused because the DWM owns the source.

That explains why `DxgkDdiPresent` never ran. The status comes from an ownership check before any DMA is built.

**Why there was no GPU redirection surface.** [INF] The KMD does not advertise GDI hardware acceleration.
`bc250-win\driver\kmd\wddm.c:2131-2146` sets only `No*` presentation caps. It leaves
`SupportKernelModeCommandBuffer` and `DriverSupportsCddDwmInterop` at zero and has no `DxgkDdiRenderKm`. See
section 2d for why that matters. [EV] The interop half of this reading is refuted: with the cap set the result is
unchanged (M677, M682). GDI hardware acceleration remains unmeasured.

Cheap tests of this reading:

- (a) `D3DKMTCheckVidPnExclusiveOwnership` on source 0 from the present device. It should return
  `VIDPN_SOURCE_IN_USE` while the DWM runs.
- (b) An ETW capture of `Microsoft-Windows-DxgKrnl` `Blit_Info` for the vkcube HWND. Look at `bRedirectedPresent`
  and whether a `PresentHistory` event follows. PresentMon itself "saw no present" in E45. With `--track_debug`
  or a raw dxgkrnl trace, the `Blit_Info` event may still appear.

## 2. Tokens, redirection surfaces and DWM-side support

### 2a. The token structures

- [DOC] `d3dkmthk.md:28948`: `D3DKMT_PRESENT.PresentHistoryToken` is "Supported starting with Windows 7"
  (`:29063-29067`).
- [DOC] The token is `{Model, TokenSize, CompositionBindingId, union Token}` (`:30058-30100`). For submission,
  "you should set TokenSize to zero" (`:30142`).
- [DOC] The token's purpose, at `:29955` and again at `:31703`: "A present history token is a data packet that
  the rendering app submits to inform the Desktop Window Manager (DWM) that rendering is complete and the swap
  chain back buffer is ready to be presented."
- [DOC] Who reads it: `D3DKMTGetPresentHistory` "retrieves copying history" (`:13090`) through
  `D3DKMT_GETPRESENTHISTORY {hAdapter, ProvidedSize, WrittenSize, pTokens, NumTokens}` (`:24789-24843`).
  `supporting-opengl-enhancements.md:54-60` lists it under "Monitoring Present History" for ICDs. [INF] The DWM is
  the consumer. No document says whether a third-party process may call it.
- [DOC] `D3DKMT_BLTMODEL_PRESENTHISTORYTOKEN {hLogicalSurface, hPhysicalSurface, EventId, DirtyRegions}`
  (`:19272-19305`). The fields are described as "handle to a logical surface to copy from" (`:19297`) and
  "handle to a physical surface to copy to" (`:19301`). No document says where these handles come from.

### 2b. Where the handles come from

The only documented source is `win32-docs\desktop-src\dwm\dwmdxgetwindowsharedsurface.md`.

- [DOC] The signature (`:24-33`):

  ```c
  HRESULT DwmDxGetWindowSharedSurface(HWND, LUID luidAdapter, HMONITOR, DWORD dwFlags,
                                      DXGI_FORMAT *pfmtWindow, HANDLE *phDxSurface, UINT64 *puiUpdateId)
  ```

- [DOC] Return `S_OK` (`:77`): "you should update the surface, being sure to pass the update ID to D3DKMTRender
  (in the PresentHistoryToken member of the D3DKMT_RENDER structure ... and then DwmDxUpdateWindowSharedSurface
  should be called with the same update ID".
- [DOC] Return `DWM_S_GDI_REDIRECTION_SURFACE` (`:78`): "you should update the surface by calling D3DKMTPresent,
  and setting PresentHistoryToken member's Model to D3DKMT_PM_REDIRECTED_BLT, and providing the update ID in the
  Blt member of the union. This value is only returned if DWM_REDIRECTION_FLAG_SUPPORT_PRESENT_TO_GDI_SURFACE was
  specified in dwFlags."
- [DOC] The API's status (`:84`): "This API is intended for implementing a graphics driver or runtime ... This
  documentation is only valid for Windows 7, and this API is not guaranteed to exist nor behave in a similar manner
  on other versions of Windows. This function is not present in any header or static-link library, and it is
  located at ordinal 100 in dwmapi.dll."
- [INF] `phDxSurface` is a shared resource handle. It is opened with `D3DKMTOpenResource` into the ICD's device, and
  that gives the `hDestination`-free target of the D3DKMTRender route. For the `REDIRECTED_BLT` route, the update
  id goes into the Blt member (probably `EventId`). It is unknown whether `hLogicalSurface`/`hPhysicalSurface` must
  be filled by the caller or by dxgkrnl.
- [DOC] The other candidate sources in the question do not produce these handles:
  - `D3DKMTCreateDCFromMemory` "creates a display context from a specified block of memory" (`d3dkmthk.md:11075`).
    It is a GDI DC over CPU memory, used by DXVK for `GetDC` (`dxvk\src\d3d11\d3d11_gdi.cpp:35`). It is not a
    logical surface.
  - `D3DKMTGetSharedPrimaryHandle` opens the shared primary: "If an OpenGL application attempts to create a primary
    surface, it typically must open the existing shared primary" (`:13373`). This is the full-screen or DWM-off
    route, and it is exactly what the DWM owns now.
  - `D3DKMTConfigureSharedResource {hDevice, hResource, IsDwm, hProcess, AllowAccess}` (`:20122-20129`) controls
    which process, including the DWM, may open a shared resource. The ICD would use it on its own shared
    allocations if the DWM opens them (flip-style routes).

### 2c. The OpenGL runtime contract (Windows 7+, still shipped in the Microsoft-provided header in Mesa)

[SRC] `mesa\src\gallium\frontends\wgl\gldrv.h` (Microsoft copyright, MIT) defines the callbacks opengl32 hands to
an ICD through `DrvSetCallbackProcs`:

- `pfnPresentBuffers(hdc, PRESENTBUFFERSCB{nVersion, syncType, luidAdapter, pPrivData, updateRect})`
  (`:395-402`).
- On Windows 10 and later, `pfnPresentToRedirectionSurface(hdc, PRESENTTOREDIRECTIONSURFACECB{nVersion, hContext,
  hSource, hDestination, hSharedHandle, updateId, updateRect, broadcast*, pPrivateDriverData})` (`:414-429`).
- `pfnSubmitPresentToRedirectionSurface` for hardware queues (`:430-449`).
- The table order is at `:454-468`.
- opengl32 calls back into the ICD with `DrvPresentBuffers(hdc, PRESENTBUFFERS{hSurface, luidAdapter,
  ullPresentToken, pPrivData})` (`:510-522`).

[SRC] Mesa's flow is in `stw_framebuffer.c`:

- `:667-681` calls `pfnPresentBuffers` with `nVersion = 2`.
- `:587-640`: `DrvPresentBuffers` opens `data->hSurface` as a shared surface (`shared_surface_open`) and calls
  `compose(..., data->ullPresentToken)`.
- `stw_winsys.h:112-123` describes `compose` as "Blit the color buffer into a shared surface" with "@sa
  GLPRESENTBUFFERSDATA::PresentHistoryToken".

[SRC] `mr-24223.md:65-76` measured this route on Windows 10/11 guests (virtio-gpu, 2023):

> "WGL: driver copies(gpu bblit) framebuffer into shared surface and submits `D3DKMTRender` with correct
> `PresentHistoryToken` and flag `PresentRedirected` / KERNEL: asks kernel-mode driver to create staging surface /
> KERNEL: submits Present with Blit from shared surface to staging surface / Something: reads back from staging
> surface into system memory / DWM: calls `ResourceUpdateSubResourceUP` to write from system memory to window
> surface"

The author was "99% sure that copy on CPU happens". jenatali confirmed it (`:86`): the dedicated DX/GL redirection
bitmap is "deprecated and is now emulated by the readback that you're seeing". The alternative is `D3DKMTPresent`
into "the GDI redirection bitmap". The author's follow-up (`:92-96`) ties that alternative to GDI hardware
acceleration.

[DOC] The render-side flags of the same route:

- `D3DKMT_RENDERFLAGS.PresentRedirected` (`d3dkmthk.md:31807-31811`) and `D3DKMT_SUBMITCOMMANDFLAGS.PresentRedirected`
  (`:33600-33602`): "a redirected present operation, which is a present to a shared allocation that belongs to the
  Display Windows Manager".
- KMD side, `DXGK_*` `RedirectedPresent` (`d3dkmddi.md:24911-24913`, `:28882-28884`).
- `D3DKMT_SUBMITCOMMAND.PresentHistoryToken` is "reserved for future use" (`d3dkmthk.md:33528-33530`). So on a
  GPUVA (WDDM 2) context the D3DKMTRender token route has no documented equivalent.

### 2d. The GDI redirection surface and what the KMD must advertise for it to live on the GPU

- [DOC] `d3dkmdt.md:6065-6090`. `D3DKMDT_GDISURFACE_TEXTURE` has these properties:
  - "not visible to the CPU, and the video memory manager will create it as a shared surface";
  - "opened by a user-mode driver and used as a texture during DWM composition";
  - "used by a user-mode driver as a render target for DirectX rendering";
  - "used as a source or destination surface in GDI hardware-accelerated operations".
- [DOC] `d3dkmddi.md:25816` (`SupportKernelModeCommandBuffer`, that is, GDI hardware acceleration through
  `DxgkDdiRenderKm`): "A display miniport driver should report that it supports GDI hardware acceleration only if
  the cache-coherent GPU aperture segment exists and there is no significant performance penalty when the CPU
  accesses the memory."
- [DOC] `d3dkmddi.md:25765` and `:25864-25866` (`DriverSupportsCddDwmInterop`): "Driver does not support hardware
  GDI acceleration, but supports Cdd-Dwm interop", that is, "Canonical Display Driver (CDD) present operations to
  texture allocations that are created by the user-mode driver for the Desktop Windows Manager (DWM) to use".
- [DOC] `d3dkmddi.md:25780`: `SupportSoftwareDeviceBitmaps` is described as "Driver supports
  D3DKMDT_GDISURFACE_TEXTURE_CPUVISIBLE redirection bitmaps". `TEXTURE_CPUVISIBLE` is "Reserved for system use"
  (`d3dkmdt.md:6184-6188`).
- [INF] Without GDI acceleration or CDD-DWM interop, the GDI redirection bitmap of a window is a CPU-side surface
  that the DWM uploads. max8rr8's measurement (`ResourceUpdateSubResourceUP` in the DWM) matches this. A
  `D3DKMTPresent` Blt then has no GPU destination. That is consistent with the E45 rejection in section 1.
- [INF, refuted] `DriverSupportsCddDwmInterop` is the cheaper of the two caps. The redirection bitmaps become textures
  created by the DWM's UMD. CDD pushes GDI content into them with `DxgkDdiPresent` blts, from CPU staging into the
  texture. An ICD's `D3DKMTPresent` Blt would then land in the same texture through `DxgkDdiPresent`. No document
  states the last step for a non-runtime ICD. It follows from PresentMon's "Composed: Copy with GPU GDI" and the
  jenatali comment.
- [EV] The previous bullet is refuted on this driver. KMD 164 sets the cap and runs a GPU DWM. The probe window
  still gets no new GDI surface, and the handshake answers `BLT_VIA_GDI` (M682). The KMT present is still refused
  at admission (M677). The archived logs show CDD's GDI context on our KMD but no GPU-backed redirection surface
  (M685). The cap does not move redirection bitmaps to the GPU here. GDI hardware acceleration
  (`SupportKernelModeCommandBuffer` with `DxgkDdiRenderKm`) is the remaining documented candidate; it is
  unmeasured.

### 2e. Each mechanism in the question

In the table, "CPU copy" means a per-frame CPU copy in steady state.

| Mechanism | Documented purpose | Windowed zero-CPU-copy route for a VRAM image while DWM composes? | DWM-side need |
|---|---|---|---|
| `D3DKMTPresent` Blt + `hWindow`, no token | ICD example, `d3dkmthk.md:15074-15097` | **Yes, one GPU blt**, but only if the window's GDI redirection surface is a GPU allocation (sections 1, 2d). E45 shows it is not today. | Redirection surfaces as GPU textures: KMD `SupportKernelModeCommandBuffer`. `DriverSupportsCddDwmInterop` is not enough (M677, M682). DWM's UMD samples a VRAM texture. No open of the ICD's allocation. |
| Same + token `D3DKMT_PM_REDIRECTED_BLT` with update id | `dwmdxgetwindowsharedsurface.md:78` | Same as above. The token only tells the DWM which update landed. Win7-only documentation. | Same as above, plus `DwmDxGetWindowSharedSurface` (dwmapi ordinal 100) returning `DWM_S_GDI_REDIRECTION_SURFACE`. |
| `D3DKMTRender` + `PresentRedirected` + token (DX/GL redirection bitmap) | `dwmdxgetwindowsharedsurface.md:77`, `RENDERFLAGS` `:31807` | **No.** It works, but the OS emulates it with a GPU-to-staging copy, a CPU readback and an upload (`mr-24223.md:65-86`). Not expressible on a GPUVA context (`SUBMITCOMMAND` token reserved). | DWM's UMD `ResourceUpdateSubresourceUP`. KMD staging blt through `DxgkDdiPresent`. |
| `D3DKMT_PM_REDIRECTED_VISTABLT` | "redirected Windows Vista bitblt" (`d3dkmthk.md:40816`, token is one ULONGLONG `:30166`) | [INF] The Vista form of the above. No documented producer. | Unknown. |
| `D3DKMT_PM_REDIRECTED_GDI`, `_GDI_SYSMEM` | GDI present history (`:24003-24090`) | No. These are win32k/CDD tokens for GDI content. PresentMon ignores `REDIRECTED_GDI` (`PresentMonTraceConsumer.cpp:906`). | N/A |
| `D3DKMT_PM_REDIRECTED_FLIP` + `Flags.RedirectedFlip` | DXGI flip model (`dxgi-flip-model.md:21`: "all back buffers are shared with the Desktop Window Manager (DWM). Therefore, the DWM can compose straight from those back buffers without any additional copy") | **Yes, zero copy**, but the token needs `hCompSurf` and composition-surface binding, which are "reserved and should be set to zero" (`d3dkmthk.md:23519-23521`). Only reachable through DXGI or the composition swapchain API. | DWM's UMD opens the app's buffers (`OpenResource` with the allocation's private data) and samples them. A hardware D3D11 UMD is needed in practice. |
| `D3DKMT_PM_REDIRECTED_COMPOSITION` | "composition swap chain ... XAML" (`:40828-40830`, token `hPrivateData` `:20087-20101`) | Same as flip, through `CreatePresentationFactory` with a D3D device (`win32-docs\desktop-src\comp_swapchain\comp-swapchain-examples.md:52`). | Same as flip. |
| `D3DKMT_PM_SURFACECOMPLETE` | `{hLogicalSurface}` (`:33879-33901`), no semantics documented | Unknown. | Unknown. |
| `D3DKMTPresentRedirected` | "Redirects the present command" (`:15220-15240`). `{hSyncObj, hDevice, WaitedFenceValue, PresentHistoryToken, Flags(reserved), hSource, pPrivateDriverData}` (`:29439-29500`) | [INF] Token-only submission with a GPU fence wait, the flip-manager era counterpart of `PresentHistoryTokenOnly`. Needs a valid token, and for flip that means the reserved composition fields. No producer documented. | As flip. |
| `Flags.RedirectedBlt` | "redirect a bitblt to a new surface" (`:29891-29895`) | [INF] The flag that marks the `REDIRECTED_BLT` token route. It is not documented as usable alone. | As the `REDIRECTED_BLT` row. |
| `Flags.PresentHistoryTokenOnly` | "the driver should submit only a present history token" (`:29951-29957`) | No pixels move. It pairs with a producer that already wrote the shared surface. | As the matching token model. |
| `hDestination` + `VidPnSourceId` + `RestrictVidPnSource` | Primary-to-primary blt by the source owner (`:29009`) and restricting a windowed blt to one output (`:29839`) | **No.** `hDestination` must be a primary and the device must own the source, which the DWM holds. `RestrictVidPnSource` only narrows the output. | N/A |
| `D3DKMTSetVidPnSourceOwner` | Ownership types at `:41365-41371`: `SHARED` "can yield to any exclusive owner, not available to legacy devices"; `EXCLUSIVE` "without shared gdi primary"; `EXCLUSIVEGDI` "only available to legacy devices"; `EMULATED` "Does not have real primary ownership, but allows the device to set gamma" | **No.** Exclusive ownership is full-screen and would evict the DWM. `SHARED` yields to the DWM's exclusive ownership. `EMULATED` grants gamma only. Taking ownership is not a windowed route. | N/A |
| `D3DKMTCheckVidPnExclusiveOwnership` | Query (`:10484-10512`) | No route. It is a **diagnostic** for the E45 status (section 1, test a). | N/A |

## 3. What open-source projects in `ref\` do for windowed presentation on WDDM

| Project | Windowed present under DWM | Copy per frame | Where |
|---|---|---|---|
| Mesa WGL frontend (`libgallium_wgl`) | `pfnPresentBuffers` callback, then `DrvPresentBuffers`: open the DWM shared surface, winsys `compose` blits into it with the present token. Otherwise winsys `present` (GDI) or a winsys framebuffer (DXGI). | GPU blit plus the OS readback (sections 2b, 2c) | `stw_framebuffer.c:587-681`, `stw_winsys.h:96-123`, `gldrv.h:395-468` |
| Mesa `wgl.c` software target | `compose = NULL`, so no redirection path: GDI blit from CPU memory | CPU | `gallium\targets\wgl\wgl.c:253` |
| Mesa d3d12 WGL winsys (GLOn12) | `IDXGIFactory::CreateSwapChainForComposition` plus DirectComposition visual, flip model | 0 to 1 GPU copy | `gallium\winsys\d3d12\wgl\d3d12_wgl_framebuffer.cpp:59-61`, `:123-140`, `:193` |
| Mesa Vulkan WSI (upstream `wsi_common_win32.cpp`) | Two image types: DXGI (`CreateSwapChainForComposition` plus DComp, `:865-909`, `:1077-1085`) for D3D12-backed drivers, and software (`CreateDIBSection` plus `StretchBlt` to the window DC, `:592`, `:849`) | DXGI: GPU. SW: CPU. | `ref\mesa\src\vulkan\wsi\wsi_common_win32.cpp` |
| Mesa d3d10umd frontend | Exports gdi32-style `D3DKMTPresent` etc. as thin wrappers for its own software runtime. It is not a present route. | N/A | `gallium\frontends\d3d10umd\D3DKMT.cpp:244`, `:417`, `:505` |
| Mesa !24223 (virgl, viogpu3d KMD) | WGL `pfnPresentBuffers` plus `D3DKMTRender` with `PresentRedirected`. D3D10 through the DXGI runtime. | GPU blit plus CPU readback (measured) | `web-docs\mesa-d3d10umd-zink-history\mr-24223.md:65-96` |
| DXVK | Uses the Vulkan driver's WSI. `D3DKMTCreateDCFromMemory` only for `GetDC` on a CPU copy. | N/A | `dxvk\src\d3d11\d3d11_gdi.cpp:35`, `d3d9\d3d9_surface.cpp:235` |
| vkd3d-proton `__WARN-LGPL-read-only` | No hits for any of the searched symbols. It relies on the Vulkan driver's WSI. | N/A | grep over `ref\` |
| VirtualBox WDDM `__WARN-GPL3-read-only` | Loads `D3DKMTPresent` and `OpenAdapterFromHdc` (`VBoxWddmUmHlp\D3DKMT.cpp:77-89`). Its D3D UMD presents through the runtime's `pfnPresentCb` (`m13-notes\zink-behind-d3d10umd.md:388-391`). No redirection-token code found. | N/A | as cited |
| Microsoft samples (ROS, COS) | `SupportKernelModeCommandBuffer = FALSE` (`graphics-driver-samples\render-only-sample\roskmd\RosKmdAdapter.cpp:1001`). No ICD present code. | N/A | as cited |
| ReactOS | Not imported into `ref\`. | N/A | N/A |
| Project fork `wt-radv` | Token-less `D3DKMTPresent(Blt, hWindow)` (E45) | would be 1 GPU blt | `src\vulkan\wsi\wsi_common_win32.cpp:128`, `:884`, `src\amd\vulkan\winsys\wddm2\radv_wddm2_wsi.c:190-195` |

Summary of section 3. No open-source project in `ref\` presents a windowed GPU image to the DWM from a non-runtime
client without either a CPU copy or a D3D11/D3D12 DXGI or composition swapchain. The zero-copy projects all
use DXGI or DComp. The token routes are either CPU-emulated (the D3DKMTRender token) or depend on GPU GDI
redirection surfaces (the D3DKMTPresent to GDI bitmap route, per jenatali).

## 4. Recommendation

Three routes, ordered by distance from today's stack.

### R1: token-less `D3DKMTPresent` Blt into a GPU GDI redirection surface

This is shortest in ICD code, because the ICD already does it (E45). It costs one GPU copy per frame and no CPU
copy. Only the KMD and the DWM's UMD are missing pieces.

Precondition to verify first:

1. Run `D3DKMTCheckVidPnExclusiveOwnership` (section 1, test a).
2. Capture a dxgkrnl ETW trace of `Blit_Info` (test b).
3. Log which `D3DKMDT_GDISURFACETYPE` values dxgkrnl passes in `DxgkDdiCreateAllocation` for
   `D3DKMDT_STANDARDALLOCATION_GDISURFACE` on unit A. The KMD accepts the type but does not log it
   (`bc250-win\driver\kmd\wddm.c`, `case D3DKMDT_STANDARDALLOCATION_GDISURFACE`).

Work per side:

- **KMD.**
  - Advertise `PresentationCaps.DriverSupportsCddDwmInterop = 1`. This is the smaller commitment than
    `SupportKernelModeCommandBuffer`, which would need `DxgkDdiRenderKm` and the full GDI op set
    (`d3dkmddi.md:27165-27167`). [EV] Done in KMD 164, and not sufficient (M677, M682).
  - Implement `DxgkDdiPresent` Blt with real DMA:
    - source `pAllocationList[DXGK_PRESENT_SOURCE_INDEX]`, destination `[DXGK_PRESENT_DESTINATION_INDEX]`
      (`d3dkmddi.md` `DxgkDdiPresent` remarks);
    - the sub-rectangle list (clipping) with `MultipassOffset` for DMA overflow;
    - the patch-location list;
    - both directions CPU staging to texture (for CDD) and VRAM to VRAM (for the ICD).
  - Accept the destination as a DWM-UMD-created texture allocation, with private data from the DWM's UMD or a
    standard allocation. This is the open question in `bc250-win\docs\design\wsi-engine-present.md:143-153`.
  - The existing ADR 0018 engine Blt is exactly this DDI.
- **DWM's UMD.** It must create the redirection textures as GPU-sampleable allocations and compose from them. The
  current CPU software D3D UMD would have to `Lock` VRAM allocations. That works only if they are CPU-visible, and
  it costs CPU reads on the DWM side. So R1 removes the CPU copy from the app but not from composition until a
  hardware D3D UMD (M13) runs the DWM.
- **ICD.**
  - Keep `D3DKMTPresent(Blt, hWindow, SubRectCnt >= 1)`.
  - Honour the drainability rule (`d3dkmthk.md:15070`: "all previously submitted work to the context(s) being
    presented from is fully drainable").
  - Read `bOptimizeForComposition` back (`:28968`).
  - The token is optional on this route. Add a `REDIRECTED_BLT` token only through `DwmDxGetWindowSharedSurface`
    with `DWM_REDIRECTION_FLAG_SUPPORT_PRESENT_TO_GDI_SURFACE`, and first check that ordinal 100 still exists on
    unit A's dwmapi.dll.

### R2: DXGI or composition swapchain, the flip model

This is the documented zero-copy route: "the DWM can compose straight from those back buffers without any
additional copy" (`dxgi-flip-model.md:21`). It needs a D3D11 or D3D12 device on the adapter, which means a
hardware D3D UMD (M13). The steps:

1. The DXGI runtime creates the swapchain buffers shared.
2. The DWM's UMD opens them with `OpenResource` and the allocation private data.
3. The Vulkan image either is the DXGI buffer (imported through `VK_KHR_external_memory_win32` KMT/NT handles) or is
   copied into it with one GPU copy. The upstream Mesa WSI takes this route (section 3).
4. The KMD needs flip-model presentation. `FlipOnVSyncMmIo` and `FlipIndependent` are already set
   (`wddm.c:2176-2181`).
5. The ICD must set `D3DKMTConfigureSharedResource`/NT-handle sharing so the DWM may open its allocations.

This is the long-term target, matching `wsi-engine-present.md` plan item 4.

### R3: CPU fallback for on-screen pixels now

`vkMapMemory` of a host-visible linear image, then `StretchDIBits` or `BitBlt` to the window DC, as upstream Mesa
software WSI does (`wsi_common_win32.cpp:592`, `:849`). The alternative is the `D3DKMTRender` +
`PresentRedirected` token route, which also costs a CPU readback in the OS (`mr-24223.md:76-86`) and has no
GPUVA-context equivalent. The existing software path in E45 already measured 900 `VK_SUCCESS` frames at a 16.8 ms
median. Keep it as the fallback.

### Which route avoids the steady-state CPU copy

- **R2** avoids every copy: the DWM samples the app's buffer.
- **R1** avoids the CPU copy on the app side and costs one GPU blt. Composition stays CPU-bound while the DWM runs
  on the software UMD.
- **R3** and the `D3DKMTRender` token route always copy on the CPU.

**Suggested order.** Run the three read-only probes of R1's precondition. Then prototype
`DriverSupportsCddDwmInterop` with the ADR 0018 engine Blt in `DxgkDdiPresent`, and watch whether dxgkrnl starts
naming a destination for the vkcube HWND (`Blit_Info` plus a `PresentHistory` `REDIRECTED_BLT` in ETW). Plan R2
with M13. [EV] The interop prototype ran: no `Blit_Info`, no redirected-blt history, and the same refusal (M677).

## Uncertain / not found

- No document lists `STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE` as a `D3DKMTPresent` return code. The routing explanation
  in section 1 is inference.
- Whether `DriverSupportsCddDwmInterop` alone, without GDI hardware acceleration, makes the DWM allocate GPU
  redirection textures for DWM-composed windows, and whether dxgkrnl then routes a third-party ICD's windowed
  `D3DKMTPresent` Blt into them. Not documented. Measure it. [EV] Measured: it does not, on this driver
  (M677, M682).
- Where `hLogicalSurface`/`hPhysicalSurface` of `D3DKMT_BLTMODEL_PRESENTHISTORYTOKEN` come from, and whether
  `EventId` is the `DwmDxGetWindowSharedSurface` update id. The documentation says "update ID in the Blt member" only.
- Whether `DwmDxGetWindowSharedSurface` (dwmapi ordinal 100) and `DwmDxUpdateWindowSharedSurface` still exist and
  behave on Windows 11 24H2. The page is Windows 7 only. Probe: `GetProcAddress(dwmapi, MAKEINTRESOURCEA(100))` on
  unit A, then call it for the vkcube HWND.
- Whether `pfnPresentToRedirectionSurface` in `gldrv.h` issues `D3DKMTPresent` with a `REDIRECTED_BLT` token on the
  ICD's behalf. The structure is published with no semantics. It is also reachable only by an OpenGL ICD loaded
  through opengl32.dll, not by a Vulkan ICD.
- `D3DKMTPresentRedirected`, `D3DKMT_PM_SURFACECOMPLETE`, `D3DKMT_PM_FLIPMANAGER` and `VISTABLT` have no documented
  producer or consumer semantics.
- Whether a non-DWM process may call `D3DKMTGetPresentHistory` usefully. It is listed for ICDs
  (`supporting-opengl-enhancements.md:54-60`) with no semantics.
- ReactOS is not imported into `ref\`, so its gdi32/D3DKMT present emulation was not checked.
- [KNOW, unverified here] On Windows 8 and later the DWM is always on, and it holds exclusive ownership of every
  VidPn source through its own swapchain. This is the ownership that the E45 status most likely reports.
