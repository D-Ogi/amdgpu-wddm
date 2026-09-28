# System D3D10/11 UMD on DXVK: integration in progress

ADR 0017 selects DXVK as the engine behind the system runtime DDI. This directory starts the new UMD integration; it is not a loadable driver, does not advertise FL11 and is not deployed. The working G0 Mesa/Zink driver remains the desktop control.

`runtime-domain.h` implements device-scoped, thread-local authority for runtime callbacks. The DDI wrapper creates a `RuntimeDomain::Scope`; the hosted dispatch callback checks the same domain in this UMD module. A DXVK worker has no scope and must marshal work back to a runtime entry instead of calling the callbacks directly. This component does not implement that marshalling queue, locking, lifetime ownership or the engine interface. A domain must outlive its scopes and engine activity; its address cannot be moved or copied. Do not duplicate this guard independently in both DLLs.

The design carries the existing G0 entry-scope semantics, including nested device entries and migration between runtime threads. It avoids binding a device to its creation thread. Tests prove denial without calling the callback for an unentered device, sibling device and worker thread, plus nesting, exception unwinding, exact callback return forwarding and subsequent runtime entry on another thread.

Build/run from the workspace:

```powershell
& .\bc250-win\tools\build\test-umd-domain.ps1
```

Current source basis for integration: Mesa UMD revision `71f2e28c1b14f60ecbe11e543279bc76ea0c1237`, hosted contract version 5; WDK 10.0.26100.0. The existing `State.h` defines `SUPPORT_D3D11 0`, and `CreateDevice` accepts D3D10/10.1 interfaces only. FL11 needs an actual D3D11 table, feature/capability negotiation and mapped DXVK operations; changing the support macro is insufficient. Shader DDI input is a token stream plus DDI signatures, so a COM boundary must account for DXBC container construction or expose an engine shader entry that accepts these inputs.

Next integration requirements: agree the versioned engine boundary; wire device creation/destruction and the runtime domain; preserve allocation/import/Present fence ownership; implement resource/shader/state/draw and FL11-specific operations; prove a system-runtime client uses the driver without app-local D3D DLLs. Image/sharing/lifecycle and the ADR performance comparison still require measurement. All lab trials remain bounded to 180 seconds.

## Input-layout translation

`input-layout.cpp` consumes actual WDK `D3D10DDIARG_CREATEELEMENTLAYOUT` entries, preserving their input register numbers, binding slots, resolved offsets and instance divisors (including zero). Binding storage is compacted without renumbering slots. Conflicting per-slot rates/divisors, duplicate registers, invalid bounds/formats and offsets that cannot fit DXVK's packed representation are rejected transactionally. The engine supplies vertex-format lookup/support information; there is no duplicated DXGI format table.

`input-layout-dxvk.h` produces actual `DxvkVertexAttribute` and `DxvkVertexBinding` arrays for the engine. WDK and DXVK headers live in separate translation units: including both exposed conflicting extern-C KMT declarations from DXVK's `util_gdi.h`. The internal data header uses Vulkan types only. This is a compiled adapter component, not a complete or versioned cross-DLL ABI; it is not yet wired into a runtime-loaded DDI table.

```powershell
& .\bc250-win\tools\build\test-umd-input-layout.ps1 -DxvkSource .\scratch\m14\dxvk
```

Validated against DXVK `52fe923ca1496c8e44789b613fab8611dfcb5c4a` and WDK 10.0.26100.0 with MSVC C++20, `/W4 /WX` for project code (`/external:W0` for upstream/WDK headers). The test passes the translated data across separate WDK and DXVK units, covering sparse slot31/register17, instance divisor zero, conflicting bindings, overflow/alignment/packing bounds and preservation of output after failure. No GPU operation is exercised by this test.

PROVENANCE: DXVK (github.com/doitsujin/dxvk), zlib; its existing vertex attribute/binding types are included from the external source checkout, not copied into this repository.

## Hosted runtime bridge

`runtime-bridge.cpp` extracts the G0 callback bridge from Mesa Device.cpp revision `71f2e28c1b14f60ecbe11e543279bc76ea0c1237`. It replaces the Gallium device dependency with runtime-owned handles, callback tables and `RuntimeDomain`. `host_descriptor` supplies the existing version-5 bootstrap descriptor (including adapter LUID) without creating a second runtime device. The ICD borrows its userdata until Vulkan destruction finishes. The caller must initialize the state, own cleanup and drain workers; the component is not yet connected to the new UMD's CreateDevice/DDI table.

The inherited operation set covers allocation, GPU VA, residency, context translation, synchronization and submission. It retains existing unsupported-operation errors; this is not a claim that every operation enumerated in the private header is implemented. Present callback orchestration is implemented by `present_runtime`; primary import and wiring into the runtime-loaded device remain at the device layer. Fault-injection switches and stderr diagnostics from the G0 bring-up copy are removed. Status/loss and submission counters remain available to the owner of the bridge.

```powershell
& .\bc250-win\tools\build\test-umd-runtime-bridge.ps1
```

The callback control builds with `/W4 /WX` for project code and checks allocation outputs, context-token translation/destruction, worker denial, wait-before-submit handling with no duplicate wait for the same Present value, pending residency translation and sticky device loss. These are controlled host callbacks, not a new hardware measurement. The private host contract is copied unchanged from the cited G0 revision.

PROVENANCE: Mesa (gitlab.freedesktop.org/mesa/mesa), MIT; callback bridge derived from the project's Mesa fork and original license retained.

## Present synchronization

The runtime bridge now exposes `queue_present_wait`, `signal_present` and `wait_present_idle`, adapted from the same G0 source. Set `RuntimeDevice::present_context` to the runtime-created presentation context. The caller flushes engine rendering, calls `queue_present_wait`, issues the actual runtime Present callback and then calls `signal_present` on successful submission. `present_runtime` now wraps these steps and invokes the actual DXGI runtime callback; the engine supplies the flush function. The engine's subsequent SubmitCommand waits for the latest Present fence value on each rendering context.

The wait aggregates the published progress of up to 16 rendering contexts. A single monitored fence is reused for Present completion. CPU waiting is isolated in `wait_present_idle`, with a 10-second bound, for teardown/readback only. The UMD still owns destruction of the fence and contexts after draining the engine. The UINT64_MAX loss sentinel is never emitted as a normal Present value. Signal failure does not advance the published value and blocks subsequent presentation work through the bridge's sticky failure flag.

The callback control includes two render queues, exact wait objects/values, fence reuse, no CPU-wait callback on the steady path, worker rejection, signal failure and sentinel exclusion. It does not establish a hardware Present result or complete M14.

`runtime-present-test.cpp` drives the actual WDK callback signatures through `present_runtime`. It verifies the order flush/create-fence/wait/Present/signal, exact allocation/context forwarding, newly published progress after flush, no later callbacks after flush/wait/Present failure, no value advance on signal failure, propagation of positive Present status and sticky device removal. Missing Present callback is rejected before engine work. This is a controlled callback test; the engine factory and DDI entry table still need to call this path before hardware validation can be claimed.

## Engine ABI r1 integration

`engine-input-layout.cpp` now calls the engine ABI's `GetVertexFormat` and `CreateInputLayout` with the translated DDI data. It includes the single contract from the external DXVK checkout (`ddi/bc250_dxvk_engine.h`), without copying the ABI header. Static assertions verify size and every field offset of the engine signature-entry and stream-output mirrors against the actual WDK structures. This compilation confirms that the r1 header can coexist with WDK headers; the earlier full-DXVK-header collision remains avoided. Engine method invocation still needs validation against the built engine DLL, not merely the local layout control.

## Engine session lifetime

`EngineSession` creates a Vulkan device from an already-created hosted instance/physical device, using only that instance's `GetInstanceProcAddr`. It queries the engine adapter level and requirements, passes the exact extension/feature chain to vkCreateDevice, obtains the requested queue and calls the ABI factory in inline mode. Requirements, service descriptors and device descriptions stay in the session until the engine is released. Instance extension-name storage, hosted bridge and engine DLL remain caller-owned and must outlive it.

Call `open` and `close` inside the runtime domain. This type has explicit teardown and must not be moved/copied. On nonzero final engine Release, `close` sets `must_retain_owner` and leaves Vulkan and requirement storage intact. The owner must retain the session, instance, runtime bridge and DLL too; it must not destroy the containing device memory. This is a leak containment path, not recovery. Integration into DDI creation/destruction is still outstanding.

```powershell
& .\\bc250-win\\tools\\build\\test-umd-engine-session.ps1 -DxvkSource .\\scratch\\m14\\dxvk
```

Controlled Vulkan and engine callbacks validate requirements propagation, queue selection, feature-chain lifetime, failures at requirement validation/Vulkan creation/engine creation, repeated close, and engine Release before Vulkan destruction. The nonzero-Release case verifies retention. No Vulkan work on a physical GPU is performed by this unit control.

## Hosted instance bootstrap

`HostedInstance` chains a copied host-v5 descriptor into vkCreateInstance using the explicitly supplied ICD entry point. It selects the physical device by valid, exact LUID equality with the D3D runtime adapter and rejects absent or ambiguous matches. Enumeration retries VK_INCOMPLETE at most three times. The shell owns the ICD module and callback userdata; they must remain alive through engine/device destruction and instance close. No implicit vulkan-1 loader or fallback adapter is used.

```powershell
& .\\bc250-win\\tools\\build\\test-umd-hosted-instance.ps1 -DxvkSource .\\scratch\\m14\\dxvk
```

Controlled ICD callbacks verify the v5 chain, non-first physical-device match, invalid LUID, absent match, duplicate match, bounded enumeration retry, create failure and exact cleanup. This tests bootstrap mechanics; a real hosted ICD and the engine DLL still need integration through the DDI CreateDevice entry. A malformed ICD lacking vkDestroyInstance leaves the handle visible for owner retention rather than silently dropping it.

## DDI device owner

`DeviceOwner::initialize` consumes the actual D3D CreateDevice arguments, copies callback tables, creates the runtime presentation context and connects HostedInstance -> EngineSession -> D3D11 device/immediate-context interfaces. The entry must already hold its RuntimeDomain scope. The owner must live on the heap; a DDI handle should store its pointer, not relocate it. It borrows both DLLs and the runtime and keeps all callback userdata stable.

`close` drains Present while the Vulkan device exists, releases the COM context/device, closes the engine/device, clears now-invalid progress pointers, destroys the instance, then destroys the Present fence/context. Failed cleanup retains the outstanding handle for retry. `has_live_objects` must be checked even after failed initialization: a failed CreateDevice is not permission to free a still-live owner or unload its DLLs.

```powershell
& .\\bc250-win\\tools\\build\\test-umd-device-owner.ps1 -DxvkSource .\\scratch\\m14\\dxvk
```

This control compiles all connected components against WDK and the engine ABI, and checks partial initialization rollback and failed context-destruction retention/retry. Successful end-to-end creation of the COM device is not covered by this control; it needs the built DXVK engine. OpenAdapter, DDI table registration and resource operations remain integration work.

## First DDI table entries

`install_draw_ddi` wires Draw, DrawIndexed, both instanced variants, DrawAuto and Dispatch to the engine's immediate context. `install_input_layout_ddi` wires layout size/create/destroy/bind through the engine ABI. The runtime private device handle stores the stable DeviceOwner pointer; each entry establishes its device domain and contains C++ exceptions. Creation errors go through SetErrorCb; child handles hold one engine COM reference. A null layout bind clears the engine binding.

These installers intentionally fill only their own implemented entries. They are not a complete D3D11.1 table, are not advertised through OpenAdapter yet and do not establish FL11 support. Typed table assignment is compiled against WDK. The control calls the entries with an uninitialized owner and verifies error reporting within the domain and restoration on return; successful draw/resource lifetime still require a real engine integration run.

```powershell
& .\\bc250-win\\tools\\build\\test-umd-ddi-draw.ps1 -DxvkSource .\\scratch\\m14\\dxvk
```

`install_raster_ddi` adds viewport/scissor arrays and primitive topology. Viewport translation preserves fractional and negative coordinates and depth endpoints; bounds checks avoid count+clear overflow. COM setters replace the complete active array, unbinding omitted slots. All base, adjacency and 1-32 control-point patch-list topology values are checked against SDK enum values at compile time before direct conversion. Tests cover translation, empty arrays, overflow rejection and typed DDI error paths; hardware rasterization remains untested on this new UMD.

`install_shader_ddi` wires VS/GS/PS create, bind and destroy through the engine shader-token entry. Shader handles retain one typed engine COM reference and record their stage; mismatched stage binding reports E_INVALIDARG. Token streams remain unchanged. The original D3D11.1 signature structure is copied field by field with Stream=0, never reading its padding as an ENTRY2 stream index. Compute, hull and domain creation/binding are now also wired; hull/domain pass the separate patch-constant signature, while compute passes empty I/O signatures. Newer stream-bearing signatures, GS stream output and class-instance setters remain to be wired explicitly. The control poisons legacy signature padding and verifies deterministic stream zero plus the existing error/domain paths; shader execution is not yet measured here.

The DDI control assigns and invokes all six stage creation/binding signatures on an uninitialized owner, verifying stage-tag initialization and failure reporting. These negative controls do not validate execution of tessellation or compute shaders; a positive engine-backed run remains required.

`install_sampler_ddi` adds sampler size/create/destroy and binding for all six shader stages. Conversion preserves filters, address modes, comparison, anisotropy, LOD and border color; compile-time checks compare mapped enum values with the SDK. Legacy TEXT_1BIT has no D3D11 equivalent and creation returns E_NOTIMPL. Slot ranges are checked before indexing; null handles become null engine sampler bindings. The local control verifies descriptor translation and typed error paths, not GPU sampling.

`install_fixed_state_ddi` supplies depth/stencil and D3D11.1 rasterizer size/create/bind/destroy callbacks. Each runtime handle owns one engine COM reference; null binding selects the COM default state. Rasterizer conversion retains signed/fractional depth bias and ForcedSampleCount. DDI FrontEnable/BackEnable have no direct COM fields: a disabled stencil face becomes ALWAYS with KEEP for all three operations, without interpreting that face's unused fields. This follows the per-face enable contract in WDK 10.0.26100 d3d10umddi.h and the local D3D10_DDI_DEPTH_STENCIL_DESC reference. Enum equality is checked at compile time. The DDI control covers poisoned disabled-face fields, distinct back-face operations, reversed face enable, global stencil disable, rasterizer translation and six additional uninitialized-engine/domain error paths. Successful COM creation and GPU depth/stencil output are still unmeasured for this shell.

`install_blend_ddi` wires D3D11.1 blend size/create/bind/destroy, including logic operations, independent targets, sample mask and blend constants. Disabled operations use canonical valid values; IndependentBlendEnable=FALSE ignores unused target descriptors. Conversion rejects simultaneous blend/logic enable and invalid masks, enums or operations without modifying the output. The later DDI ALPHA_FACTOR/INVALPHA_FACTOR values currently return E_NOTIMPL when active; their semantics are not replaced by a color blend factor. Tests exercise poisoned ignored targets, independent XOR and write masks, failure without output mutation, and typed uninitialized-engine error paths. These controls do not measure GPU blending through the system UMD.

`install_resource_ddi` adds ordinary engine-owned buffer (including BUFFEREX), 1D/2D/3D and cube texture creation/destruction. DDI CPU access, UAV and misc bits are translated explicitly, not cast to COM flags. Logical texel dimensions are used instead of padded physical extents; declared mip chains are checked, array faces remain individual subresources and initial row/slice pitches are copied. Structured buffer stride and cube misc flags are preserved. The D3D11.1 callback never reads fields added by later DDI versions. Shared resources, Present resources and primary descriptors return E_NOTIMPL until the runtime allocation/import path is connected; an ordinary private engine texture is not a substitute. Other unsupported bind/misc bits also fail explicitly. The converter leaves its output unchanged on rejection; allocation exceptions are caught by the DDI entry wrapper.

`install_buffer_binding_ddi` adds vertex/index binding and all six ConstantBuffers1 setters. Null handles unbind; non-buffer resource handles are rejected before the COM call. Slot bounds are checked without addition overflow. D3D11.1 optional first/count arrays are forwarded in constant units, not bytes. COM state retains its own references; the runtime handle owns the creation reference.

The local control now covers padded versus logical extents, volume mips, array initial-data count/pitches, cube faces, shifted UAV/CPU/structured flags, invalid sizes, explicit shared/Present rejection and actual typed callback assignments. Calls with an uninitialized engine verify error/domain behavior. These checks are not a positive render through the DDI shell. Views, mapping/update/copy, shared imports, OpenAdapter and full table/capability publication remain open integration work.

`install_transfer_ddi` connects CopyResource, CopySubresourceRegion1, UpdateSubresource1 and ResolveSubresource. Signed DDI boxes are converted field by field, negative coordinates are rejected, and empty boxes do not issue work. Null boxes remain null. Source/destination subresources, destination coordinates, format and update row/slice pitches are forwarded. NO_OVERWRITE and DISCARD map explicitly; contradictory or unknown flags fail. TILEABLE is a permission for a tile-renderer optimization (WDK D3D11_1_DDI_COPY_FLAGS), so the full ordinary operation is used without that hint. There is no shell readback or CPU full-frame copy in these adapters. The control verifies box/flag translation and typed uninitialized-engine error paths; a hardware transfer through the completed system UMD remains untested.

`install_map_ddi` adds general, staging and dynamic IA/constant/texture Map/Unmap entries. Five DDI map modes and DO_NOT_WAIT are converted explicitly. Output pointer and pitches are cleared before entering the engine, including failure on an uninitialized context. COM Map errors (including WAS_STILL_DRAWING and device removal) go to SetErrorCb without retries; successful results preserve row/depth pitch. The shell introduces no extra wait or copy. The engine remains responsible for DISCARD renaming and resource synchronization. A successful Map with an unexpectedly null pointer is balanced by Unmap and reported as failure. The default constant-buffer UpdateSubresourceUP slot now uses the same transfer implementation as the general update slot. The local control covers mode/flag conversion and all typed entry error paths; actual mapped data and synchronization through the system UMD are not yet measured.

`install_rtv_ddi` adds render-target view size/create/destroy/clear for buffers, 1D/2D/3D textures, cube faces and MSAA. Creation obtains the engine resource's actual type, array size and sample count before selecting a COM view dimension. Cube faces become 2D array layers; volume slices remain W slices. Range checks avoid first+count overflow; invalid MSAA mip selection fails before creation. The handle owns one engine view reference, while the view retains its resource. Local tests cover array/mip fields, MSAA and non-MSAA variants, cube arrays, volumes, buffers, overflow rejection and typed error paths. Render-target binding alongside depth/UAV views remains a separate integration step, and the tests do not show GPU rendering through these DDI entries.

`install_dsv_ddi` adds depth/stencil view size/create/destroy/clear. 1D, 2D, array, cube-face and MSAA selection shares RTV layer validation with explicit conversion between distinct view structures and enums. Actual resource type/sample count are obtained from the engine before creation. Depth/stencil read-only bits and clear bits are independently translated; unknown flags and unsupported dimensions fail. Local controls cover both read-only flags, array/MSAA/cube conversion, invalid mip/dimension rejection and typed error paths. These controls do not establish GPU depth testing through the system UMD; combined output binding still needs implementation.

`install_uav_ddi` adds unordered-access view creation/destruction, uint/float clear, compute binding and CopyStructureCount. Buffer RAW/APPEND/COUNTER flags map explicitly, incompatible combinations fail, and texture views reuse validated mip/layer selection. MSAA and direct cube UAV descriptors are rejected (cube storage uses 2D face views in DDI). Compute slot bounds use the D3D11.1 limit; null handles unbind and initial counters pass unchanged, including UINT_MAX. Counter copies require a buffer and a four-byte-aligned offset. Local controls cover descriptors, flags, array overflow, MSAA rejection and typed error paths; compute execution and counter values through the system UMD remain unmeasured.

`install_output_ddi` connects the combined RTV/DSV/UAV output-merger state. The local WDK SetRenderTargets(D3D11) contract supplies complete bindings; UAVRangeStart/Size are optimization hints. Preparation therefore fills all UAV slots after the active RTV range, explicitly nulling omitted bindings so COM cannot retain stale outputs. Unused counters remain UINT_MAX, supplied values are preserved, and null RTV/DSV handles unbind. The entry uses 8 or 64 UAV slots according to the engine feature level. Local controls test sparse bindings, a narrow changed-range hint, RTV-only clearing, last-slot binding, count overflow/overlap and exact opaque view identities without COM ownership changes. The GPU output-merger path through the full system UMD remains unmeasured.
