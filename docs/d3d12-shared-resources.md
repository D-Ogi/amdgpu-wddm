# D3D12 shared resources: state and plan

This document records what the D3D12 driver does with a shared resource, how the shared surface is described on
the wire, and what still refuses. It answers the defect BD-075.

## What works and what does not

Shared fences pass in every direction. The D3D12 UMD DDI has no fence sharing in it. `D3D12DDI_FENCE`
(`d3d12umddi.h`) holds two GPU addresses and one flag bit, and the flag is `BOTTOM_OF_PIPE`. The kernel
object, its NT handle, `CreateSharedHandle` and `OpenSharedHandle` all belong to the runtime. A shared fence
and a local fence look the same at the DDI, so the driver needs no code for the difference.

Shared resources work for one shape: a 2D texture of one mip level, one array slice and one sample, on a
GPU-only heap, in a format that has a COMPOSED row in `driver/contract/amdgpu_wddm_surface_format.h`. That is
the shape a second driver can read, because both sides compute the same addresses from the same table.

| Call | Slot | What happens |
|---|---|---|
| `CreateCommittedResource` with `D3D12_HEAP_FLAG_SHARED` | D60 `pfnCreateHeapAndResource` | Two attempts. The runtime refuses the first, which is the ordinary create. The second describes a linear surface and publishes the two private-data records. |
| `ID3D12CompatibilityDevice::CreateSharedResource` (keyed mutex) | D60, the same pair | The same two attempts. The keyed mutex itself is a kernel object of the runtime. |
| `ID3D12Device::OpenSharedHandle` of a resource | D65 `pfnOpenHeapAndResource` | The shell adopts the allocation the runtime opened, decodes the records, and places the image in that memory. |

Everything outside the shape above still refuses, and so does a shared heap with no resource. The refusals are
listed under "What refuses" below.

## A refusal must not remove the device

A creation function of a user-mode display driver is in the AllowOutOfMemory category (`windows-driver-docs`
display `handling-errors.md`). The runtime admits `E_OUTOFMEMORY` and `D3DDDIERR_DEVICEREMOVED` from it. It
treats any other failure as a critical driver failure, removes the device, and sets the removed reason to
`DXGI_ERROR_DRIVER_INTERNAL_ERROR`. The driver answered `E_NOTIMPL`, so all 13 shared-resource cells of
`tools/win/capture-share` reported `0x887A0005` with reason `0x887A0020`.

`engine_ddi::admitted_create_failure` is the last step of both slots, and of the shell's own wrapper of D60. A
refusal reports `E_OUTOFMEMORY`, and the `log_refusal` line of the same call keeps the real HRESULT. The device
stays alive.

Three layers can refuse one create, and the runtime cannot tell them apart, so each clamps what it decides:

| Layer | Refuses with | Clamp |
|---|---|---|
| the DDI thunk the runtime calls (`ddi-entry.h`) | `E_INVALIDARG` for a handle that resolves to nothing, `E_UNEXPECTED` for a scope it could not enter or a missing original, `E_FAIL` for an exception | `native12::ddi_admitted_create_failure`, for the bindings that opt in (`CoreBinding::allow_out_of_memory`) |
| the shell's wrapper of D60 (`native-tables.cpp`) | `E_UNEXPECTED`, and whatever the owner scope decides | `engine_ddi::admitted_create_failure` |
| the slots themselves (`engine-ddi/resources.cpp`) | the heap-import refusal, `E_INVALIDARG`, `E_NOTIMPL` | `engine_ddi::admitted_create_failure` |

A lost device is the one other admitted answer, and only under the name the runtime admits. Both clamps turn
`DXGI_ERROR_DEVICE_REMOVED`, `_RESET` and `_HUNG` into `D3DDDIERR_DEVICEREMOVED`: the DXGI codes are what the
API shows the application, not what the AllowOutOfMemory list admits, and the driver reaches them on live paths
(`create_heap_and_resource` for a context already lost, `heap-import.cpp` for `VK_ERROR_DEVICE_LOST`). Reporting
one of them from a create slot would remove the device again, with `DRIVER_INTERNAL_ERROR` over the real reason -
the BD-075 signature itself. `native-tables.cpp` holds the two clamps to the same answer with `static_assert`s.

The one code that is not a refusal is `engine_ddi::kShareRequired` (`0xA0BC2075`). It never leaves the shell: it
travels from `RuntimeHeapImports::allocate` to `create_heap_and_resource`, which answers it with the second
attempt. `admitted_create_failure` maps it to `E_OUTOFMEMORY`, so a path that ever returned it to the runtime
would report a refusal, not a strange status. A header test pins that.

## What the measurement said

The lab trial of 2026-10-05 (`lab-20261005T172552Z`, `ddi.log`, 152570 lines) settled the question the plan was
blocked on. For the shared create of a 256x256 `B8G8R8A8_UNORM` texture, at line 50860:

```
{"event":"allocate-callback","edge":"begin","allocations":1,"runtime_resource":1,"kernel_resource":0,
 "private_size":0,"info_flags":0,"source":0,"info_private_size":192,...}
{"event":"allocate-callback","edge":"end","status":"80070057","allocation":0,...}
engine-ddi: CreateHeapAndResource: 80070057 reported as 8007000e; heap description given (262144 bytes,
 alignment 65536, flags 0x26, pool 1, cpu page 0), resource description given (type 3, 256x256, depth 1,
 mips 1, format 87, samples 1, layout 0, flags 0x11, castable 0)
```

Three facts come out of it:

- The heap flags are `0x26` - `BUFFERS | NON_RT_DS_TEXTURES | RT_DS_TEXTURES`. Those are an ordinary committed
  texture's flags. The runtime adds no bit for sharing, so none of the three candidate bits of the earlier plan
  was right, and no heap flag can be read as "this resource is shared".
- `pfnAllocateCb` itself refuses with `E_INVALIDARG`. The call carried `private_size` 0, which is the
  resource-level private data: an ordinary D3D12 create publishes none.
- 686 of the 691 allocate callbacks of that trial succeeded. The five that failed are exactly the five shared
  creates of the run. Nothing else in the trial was refused.

So the runtime refuses a shared resource whose allocation carries no resource-level private data. The refusal
is the only signal the DDI gives, and the create reads it in the one place where it can mean that.

## The create, as built

`create_heap_and_resource` (`engine-ddi/resources.cpp`) makes at most two attempts.

Attempt 1 is the ordinary create it always made, with one addition: inside the shareable envelope the memory
request carries `kMemoryShareable` (`0x8`) beside `kMemoryDedicated`. Nothing about the allocation changes. The
flag exists so that `RuntimeHeapImports::allocate` can tell the runtime's refusal of a shared resource from any
other `E_INVALIDARG`: when the allocate callback answers `E_INVALIDARG` for a shareable request that published
no surface record, the shell answers `kShareRequired`.

The shareable envelope is the shape attempt 2 can describe: `TEXTURE2D`, one mip, one slice, one sample, a
GPU-only heap that is not system-wide coherent, a width and height within 8192, and a format with a COMPOSED
row. A create outside it never carries `kMemoryShareable`, so it can never be retried, and its refusals are
unchanged.

Attempt 2 asks the engine for the image's linear layout (`QueryLinearImage`), replaces the request's `ByteSize`
with the linear backing size and its `Alignment` with 0, and imports with
`kMemoryDedicated | kMemoryShareable | kMemoryLinearSurface`. The resource is then placed in that memory with
`CreateLinearPlacedResource` at offset 0, as a linear primary is.

What the create publishes, through `AllocationRequest::prepare_shared_surface`:

| Blob | Size | Contents |
|---|---|---|
| allocation private data | 32 bytes | `BC250_WDDM_ALLOCATION_PRIVATE`, "LB7A" v1: Width, Height, Pitch, Format (a `D3DDDIFORMAT`), Size |
| resource private data | 64 bytes | `BC250_SURFACE_RESOURCE_PRIVATE`, "E26R" v3: `Shared` 1, `Access` 0, and a serialized `D3D11_TEXTURE2D_DESC1` |

`D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE` and `VidPnSourceId` 0: a shared texture is not a primary. The pitch
is the engine's row pitch, a multiple of 16 bytes on this part. The size is
`max(RowPitch * roundup4(Height), MemorySize)`, page rounded. The 192-byte `bc250_umd_alloc_private` blob of an
ordinary allocation is not sent: the kernel driver places an LB7A surface from the LB7A description.

`BindFlags` in the record names what an opener may build over the surface. The DDI states resource flags
positively, so the three the record can carry are mapped one to one and nothing else is:
`SHADER_RESOURCE` to `D3D11_BIND_SHADER_RESOURCE`, `RENDER_TARGET` to `D3D11_BIND_RENDER_TARGET`,
`UNORDERED_ACCESS` to `D3D11_BIND_UNORDERED_ACCESS`. A flag outside those three (`SIMULTANEOUS_ACCESS`, for
instance) is published as no view at all.

The kernel driver needs no change. `Bc250SurfaceResourcePolicy` already admits v3, and `WddmCreateAdmit`
already places a shared type-0 surface in the aperture segment.

## One reader and one writer of the wire format

`driver/contract/bc250_shared_surface.h` is the only code that writes or reads the pair of records. It holds
the geometry rule, the format lookup, the encoder and two decoders, in plain integers and with no API header,
so the kernel driver's C, both shells' C++ and the Mesa winsys's C can all include it.

The two decoders differ in policy alone:

- `Bc250SharedSurfaceDecode` is the D3D12 shell's. It admits a shared texture and nothing else: `Shared` 1 and
  no access intent.
- `Bc250SharedSurfaceDecodeAdmitted` takes the opener's policy. The D3D11 shell passes the whole access mask,
  because the compositor hands it primaries and surfaces that ask for a cached CPU mapping, and refusing those
  would close the desktop route.

Before this header each shell carried its own copy of the rules, so a D3D12 surface and a D3D11 surface could
differ in a field neither side checked. `decode_open_resource` in the D3D11 shell is now a thin wrapper of the
admitted decoder, and `runtime_surface_geometry` calls the header's rule.

## The open, as built

`open_heap_and_resource` (`engine-ddi/resources.cpp`) does six things, and refuses before any of them if the
open is not the shape above.

1. It checks the open: one allocation, a 32-byte allocation blob and a 64-byte resource blob.
2. It decodes them with `Bc250SharedSurfaceDecode`.
3. It builds a `D3D12_RESOURCE_DESC1` from the decoded record: the width, the height, the DXGI format, one mip,
   one slice, one sample, and the views the record's `BindFlags` name.
4. It asks the engine for that image's linear layout and refuses unless the layout agrees with the record:
   `info.RowPitch == record.Pitch` and the backing size is within `record.Size`. The tiling agreement is
   enforced, never assumed. A creator and an opener that disagree would read different addresses in the same
   memory, which is the worst failure this path can have.
5. It adopts the allocation through the new `ShellHooks::adopt_memory`, which calls
   `RuntimeHeapImports::adopt`. There is no allocate callback: the allocation exists. Everything after it is
   the work `allocate` does, in the same order and for the same reasons (trial 153: the GPU address must not
   reach the engine before the allocation is resident).
6. It creates a heap over that memory (`CreateHeapFromMemory`) and places the resource in it at offset 0, with
   the open's `InitialResourceState`. The resource is recorded as `ResourceKind::Opened`.

An adopted allocation is borrowed, and the lifetime rules say so in four places. Its record never calls
`pfnDeallocateCb`, because this driver did not create it and the runtime destroys it with the opened resource.
`RuntimeAllocation::adopt` sets `borrowed_`, and `close` with the owner form is refused rather than silently
turned into the handle form. `owns_allocation` answers false, so the ICD's borrowed-allocation map does not
take it for one of ours. `borrow_backing` refuses it, so no second view is placed beside the opened image. It
is never quarantined: a mapping held past the destroy would name memory that is no longer the opener's.
`FreeReport::adopted` says which release it was.

## The keyed mutex

The keyed mutex needs no code in this driver. `IDXGIKeyedMutex` is the runtime's object, and D3D11On12 reaches
it at the first `Acquire`. The `km12to11` and `km11to12` cells use the same two records as the plain shared
cells, so they are expected to work with the create and the open above and nothing else.

## What refuses

Every refusal below reports `E_OUTOFMEMORY` to the runtime, names its own check in the driver log
(`heap-import.h`, `ImportReport::refusal`) and leaves the device alive.

| Asked for | Refusal |
|---|---|
| a shared buffer, mip chain, array, MSAA or 3D texture | `shared surface shape` |
| a format with no COMPOSED row, including block-compressed formats | `shared surface format` |
| a shared texture on a CPU-visible or system-wide coherent heap | `shared surface shape` |
| a shared texture on a `PRIMARY` heap | `heap flags`, with the bit in the report |
| a shared heap with no resource, to place resources in later | `shareable heap without a resource` |
| a placed resource on an opened or shared surface | `placed resource on a linear surface` |
| an open of more than one allocation, or with a blob of the wrong size | `opened surface records` |
| an open whose record and engine layout disagree | `opened surface layout` |
| `SHARED_CROSS_ADAPTER` | the heap flags check: this driver has one adapter |

## The reported compatibility tier

`D3D12_FEATURE_DATA_D3D12_OPTIONS4::SharedResourceCompatibilityTier` still cannot be made truthful from this
driver. There is no DDI field for it. The name appears nowhere in `d3d12umddi.h`, and
`D3D12DDI_D3D12_OPTIONS_DATA_0089` does not carry it. The D3D12 runtime builds the value from the WDDM and DDI
level the driver reports. Tier 1 support "is built into WDDM 2.4" (`ref/sdk-api-docs`
`ne-d3d12-d3d12_shared_resource_compatibility_tier.md`), and this driver reports DDI interface `0xE003`, which
is WDDM 2.9.

The arithmetic of what the reported tier 2 promises and what the driver now holds, from the same SDK page. Tier 0
is eight formats: `R8G8B8A8_UNORM` and its sRGB form, `B8G8R8A8_UNORM` and its sRGB form, `B8G8R8X8_UNORM` and
its sRGB form, `R10G10B10A2_UNORM` and `R16G16B16A16_FLOAT`. Tier 1 adds nine typeless formats, tier 2 adds
`NV12`, tier 3 adds `R11G11B10_FLOAT`. The five COMPOSED rows of the surface format table cover six of tier 0's
eight - the two `B8G8R8X8` forms have no DXGI entry in our table - and none of tier 1's or tier 2's. The driver
also shares no buffer, and `A8_UNORM` is one row it shares that no tier asks for.

Tier 2 is a format promise and nothing else. The earlier note here that it "adds keyed-mutex shapes" is not in
the specification: the keyed mutex is the runtime's object at every tier (see "The keyed mutex" above). The
honest answer is this document plus the refusals above: an application that asks for a shape the table does not
hold is told that the driver cannot do it, in a code the runtime survives.

## Order of work

1. Done: refuse without removing the device, name the refusal, and report the open slots as real slots.
2. Done: the measurement that said what a shared create looks like at the DDI.
3. Done: the create and the open, the shared wire format in one header, and the host tests below.
4. Next: the lab trial. `scratch/train/b19-bd075/lab-bd075-real.ps1` (local) runs the shared cells of
   `tools/win/capture-share` in two passes, each inside the three-minute bound. Every shared cell must end in
   `result=pass` with its three content oracles, and the injected negative control must still end in
   `mismatch` with a violated gate.
5. Later: the shapes the table does not hold (tiled and multi-mip sharing need a deterministic layout, which
   `check_resource_allocation_info` does not implement), cross-adapter sharing, and the reported tier.

## Tests

| Test | What it pins |
|---|---|
| `driver/contract/test/shared-surface-test.cpp` | the two records as byte literals: 44 named checks over the encoder, both decoders, the kernel driver's parser and every single-field refusal, the last of which covers all 5 COMPOSED rows and their 7 format round trips |
| `driver/umd/d3d12/allocation-request-test.cpp` | what the create publishes, field by field, and that `prepare_surface` still writes the primary's v1 record |
| `driver/umd/d3d12/heap-import-test.cpp` | the shell half: the shareable envelope, `kShareRequired` only from the runtime's refusal, the shared surface's records and refusals, and the borrowed lifetime of an adopted allocation |
| `engine-ddi/tests/test-shared-create.cpp` | round trip 8 on the GPU: the retry, the geometry the engine measured, a texel-exact render and read-back of the created surface |
| `engine-ddi/tests/test-shared-open.cpp` | round trip 9 on the GPU: one adopt, a texel-exact clear and read-back of the opened surface, and 12 malformed records. Also the two shells that serve no adopt |
| `engine-ddi/engine-ddi-header-test.cpp` | boundary revision 5, the `AdoptRequest` layout, and `kShareRequired` clamped to `E_OUTOFMEMORY` |

## Open questions for later work

- The engine carries upstream vkd3d-proton's full Win32 sharing path for committed resources
  (`libs/vkd3d/resource.c`, `libs/vkd3d/d3dkmt.c`). None of it runs. The shell allocates every heap itself and
  calls `CreateHeapFromMemory`, whose validator refuses `D3D12_HEAP_FLAG_SHARED` by design. If this ever moves
  to the engine's path instead, the ICD needs work first: the WDDM winsys cannot export at all
  (`radv_wddm2_bo_get_handle` returns false, so `vkGetMemoryWin32HandleKHR` fails), while the physical device
  advertises `VK_KHR_external_memory_win32` and answers `EXPORTABLE` for buffers and images. The ICD's NT
  handle import also accepts one linear 8-bit surface shape only. These advertised caps are not truthful, and
  nothing on the deployed path reads them today.
- `admitted_create_failure` covers the heap and resource create and open slots. The other create slots of the
  core table still report whatever they decide. An audit of all of them belongs in its own change, with the
  host tests that pin each slot's admitted failures. The audit is not a formality: a slot that reports
  `E_INVALIDARG` for a malformed argument tells the debug layer something true, and clamping it to
  `E_OUTOFMEMORY` would take that away. Each slot needs the question asked once: which of its failures can the
  runtime survive, and which of them is a lie if it is called out of memory.
- A clamped refusal is quiet. It used to be unmissable - the device went away and the reason said
  `DRIVER_INTERNAL_ERROR` - and now it is one line on a channel that a game trial does not read
  (`AMDGPU_WDDM_LOG` unset, no debugger, no DBWIN listener), while the application reads `E_OUTOFMEMORY` as "out
  of video memory" and may quietly drop settings. The driver has no always-on counter surface of its own for a
  trial to read, so a count of clamped refusals per device belongs with the overlay's graphics row or
  `bc250kmd_cli`, in the change that gives the UMD that surface. Until then the trial that must see them passes
  `-Trace` (`AMDGPU_WDDM_LOG` plus `AMDGPU_WDDM_DDI_TRACE`), and `tools/win/capture-share` records the debugger
  channel of both of its processes without any switch.
- `check_resource_allocation_info` ignores `D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_DETERMINISTIC`. It answers a
  vendor-swizzled layout for a request that asks for a reproducible one. That is what a tiled shared texture
  would need, and it is why the shared shape above is linear only. The flag is not a sharing marker either: in
  the trace of `20261005T172552Z` the runtime set `DETERMINISTIC` with `SIMULTANEOUS_ACCESS` (optimization 9
  beside resource flags 0x19) and did not set it for the shared creates, whose optimization flags were `1`
  (`SHADER_RESOURCE`) alone.
- Whether the D3D12 runtime expects the opener to release its view of an adopted allocation. This driver makes
  no deallocate callback for one, because it never allocated it, while the D3D11 shell does call
  `pfnDeallocate2Cb` with the runtime resource handle for its adopted surfaces and works on the lab. The two
  runtimes are not the same: a D3D9/10/11 `OpenResource` hands the UMD a counted view that it closes, and the
  D3D12 open hands it a handle it never owned. If the lab shows otherwise - a second open of the same handle
  failing, or an allocation count that grows across opens - the fix is one line, `close()` by handle list for an
  adopted record, and the trial that shows it is a repeat of one cell inside one process.
- The two attempts of a shared create cost one refused allocate callback each. The runtime's refusal is cheap
  (no allocation is made), and a create is not a per-frame path, so nothing is measured here yet. If a title
  ever creates shared surfaces in a loop, the shell could remember that this device needs the surface record
  and skip attempt 1. It would be a device-wide guess, so it needs a measurement first.
