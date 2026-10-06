# D3D12 shared resources: state and plan

This document records what the D3D12 driver does with a shared resource, how the shared surface is described on
the wire, and what still refuses. It answers the defect BD-075.

## What works and what does not

Shared fences pass in every direction. The D3D12 UMD DDI has no fence sharing in it. `D3D12DDI_FENCE`
(`d3d12umddi.h`) holds two GPU addresses and one flag bit, and the flag is `BOTTOM_OF_PIPE`. The kernel
object, its NT handle, `CreateSharedHandle` and `OpenSharedHandle` all belong to the runtime. A shared fence
and a local fence look the same at the DDI, so the driver needs no code for the difference.

Shared resources work for one shape: a 2D texture of one mip level, one array slice and one sample, on a
GPU-only heap, in a format that has a COMPOSED row in `driver/contract/amdgpu_wddm_surface_format.h`, or in the
sRGB view that shares that row's storage. That is the shape a second driver can read, because both sides compute
the same addresses from the same table.

One lookup answers "is this format a shared surface's" for the whole stack: `Bc250SharedSurfaceFormat` in
`driver/contract/bc250_shared_surface.h`, which the wire's encoder, the D3D11 shell's `runtime_surface_format`
and the D3D12 shell's `composed_format` all call. Until 2026-10-06 the D3D12 half called
`amdgpu_wddm_surface_format_by_dxgi` instead, which matches the storage column alone, so an sRGB surface was
admitted by the wire and refused by this driver: a D3D11-created `B8G8R8A8_UNORM_SRGB` shared texture decoded and
then failed in `query_linear_surface`, and an sRGB shared create never reached its retry at all. The review that
found it also found why nothing caught it - the blob layer's tests round-trip both sRGB views, so the wire looked
covered, and `capture-share` had no `--format` value that could ask for one.

The shell's own admission gate was one caller behind until 2026-10-06: `RuntimeHeapImports::allocate`
(`driver/umd/d3d12/heap-import.cpp`) still looked a shared surface's format up with
`amdgpu_wddm_surface_format_by_dxgi`, so the lab's `s12to11-srgb` row reached the retry, had its description
admitted by `engine-ddi`, and was then refused by the shell with `shared surface format` and `E_NOTIMPL` -
reported to the application as `E_OUTOFMEMORY` from `CreateCommittedResource`. The gate now uses
`Bc250SharedSurfaceFormat` for a shared surface. A primary keeps the storage column, because no `D3DDDIFORMAT`
expresses an sRGB view and the `SCANOUT_PRIMARY` `X8` row has no DXGI format at all.

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
GPU-only heap that is not system-wide coherent, a width and height within 8192, and a format with a COMPOSED row
or the sRGB view of one. A create outside it never carries `kMemoryShareable`, so it can never be retried, and
its refusals are unchanged.

What keeps the retry from masking a real refusal is the narrow discriminator and the envelope, and nothing else.
An earlier version of this document and of the comment in `resources.cpp` claimed that attempt 2 is strictly more
constrained than attempt 1, so that a request failing the first for a size or an alignment reason fails the
second as well. That is wrong: attempt 2 asks for a different segment (the aperture, which the LB7A and E26R
blobs select through `Bc250SurfaceResourcePolicy`), a different size and a different alignment. The load-bearing
check is `share_required` in `heap-import.cpp` - `E_INVALIDARG` alone, from the allocate callback of a first
attempt inside the envelope that published no resource-level private data.

Because the envelope holds every single-mip 2D BGRA8, RGBA8, RGB10A2, RGBA16F and A8 texture up to 8192 on a
DEFAULT heap, a retry firing where it should not would turn an ordinary committed texture linear and move it to
the GTT aperture with no error anywhere. Two things make that visible and reversible:

- every retry is counted (`engine_ddi::shared_surface_retries`) and names itself through `log_refusal`, which
  reaches the debugger channel with no trace build and under the refusal budget. The lab trial scores those
  counts per cell, and its controls must show zero.
- `AMDGPU_WDDM_D3D12_EXPERIMENT=shared-create-retry-off` turns the whole behaviour off: the shell answers the
  runtime's own status instead of `kShareRequired`, so nothing is ever retried. The fail-fast part - a refusal
  that does not remove the device - has no switch and is not meant to have one.

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

An adopted allocation is borrowed, and the lifetime rules say so in five places. Its record never calls
`pfnDeallocateCb`, because this driver did not create it and the runtime destroys it with the opened resource.
`RuntimeAllocation::adopt` sets `borrowed_`, and `close` with the owner form is refused rather than silently
turned into the handle form. `borrow_backing` refuses it, so no second view is placed beside the opened image. It
is never quarantined: a mapping held past the destroy would name memory that is no longer the opener's. Its
residency reference is dropped without an `Evict` callback, because the runtime's destroy can outrun our release
and the callback would name a handle dxgkrnl may already recycle. Destroying an allocation drops its residency
anyway. `FreeReport::adopted` says which release it was.

`owns_allocation` is not one of those places, although it read as one until 2026-10-06. Its only caller is the
`Lock2`/`Unlock2` router in `device-engine.cpp`, and the question it answers is "is this allocation in our store",
so that the lock reaches our record and its own state checks instead of `HostedDispatch`, which refuses an
allocation it does not know. A borrowed allocation is in the store like any other. The question of whose
allocation it is to destroy lives in `release_owned`.

The same handle may be adopted more than once. An application may call `OpenSharedHandle` twice on one device for
the same NT handle, and nothing we can read promises that dxgkrnl answers with two different allocation handles.
`engine-ddi` asks the shell once per open either way. The store therefore keeps one record per open and tells
their imports apart by the cookie each `ImportedMemory` carries, not by the handle. A handle this driver created
itself is still refused ("the allocation is one this driver created"), which is the store corruption the first
form of that check was written for.

An opened surface is a legal destination of a blt-model Present and is not a legal answer to the destroy slot's
"whose allocation is this to release". Those are two questions, and `engine-ddi.h` has two entry points for them:
`present_destination_allocation` admits `ResourceKind::Opened`, `present_allocation` does not. Before BD-075 no
opened resource could exist, the two shared one answer, and naming an opened resource as the Present destination
would have failed at stage 4 and removed the device.

## The keyed mutex

The keyed mutex needs no code in this driver. `IDXGIKeyedMutex` is the runtime's object, and D3D11On12 reaches
it at the first `Acquire`. The `km12to11` and `km11to12` cells use the same two records as the plain shared
cells, so they are expected to work with the create and the open above and nothing else.

### The handover, and what is not proven about it

"No code in this driver" is true of the mutex object and is not true of the ordering it implies. A D3D12 creator
writes the surface with `ExecuteCommandLists` and then hands the key over with `ReleaseSync`, and the reader is
entitled to the written pixels. Nothing in this driver places the runtime's kernel-side release behind that
submission:

- the two queue slots that could, `pfnSignalFence` and `pfnWaitForFence`
  (`driver/umd/d3d12/native-queue-ddi.cpp`), only select the physical adapters of a broadcast.
  `PhysicalAdapterMask` is an out parameter - "the set of adapters to broadcast the operation to"
  (`d3d12umddi.h`) - so both slots answer with this single node and perform no fence operation. What is measured
  is that the system runtime completes a single-adapter operation without them: `w12`'s `w12-pending` control
  queues an `ID3D12CommandQueue::Wait` on a value nobody has signalled yet and finds the dependent signal still
  unreached 300 ms later. What is **not** measured is a fence another process opened: neither slot appears in the
  eleven traced rows of 2026-10-06, but none of those rows waits on an opened shared fence (`s11to12-fence` died
  at `open-shared`, `f12to12` ran untraced), so "never called" does not cover the cross-process case. Each slot
  therefore records its first call per process on the always-on debugger channel (`fence_slot_seen`), and the lab
  kit counts both slots per row, so the next pass answers this with a number instead of an absence;
- `pfnSignalFence`'s own comment states the precondition it depends on - "Execute must already have submitted all
  work on that same context" - and `submit_locked` (`engine-ddi/queue.cpp`) forwards to vkd3d-proton's
  `ExecuteCommandLists`, which may hand the work to its submission thread and return. The precondition is
  therefore not guaranteed, only usually met.

The lab's `km12to11` row is the open question: its reader blocks in `AcquireSync(3)` (a real CPU block, the gate
proves it did not acquire early) and still reads poison, while the plain `s12to11` row passes - and the one
difference in the client is that `s12to11`'s creator calls `Finish()`, a CPU wait on its own work, before it tells
the reader. The discriminator is `capshare --creator-finish`, which gives the keyed-mutex creator that same wait
before every `ReleaseSync`. If the row then passes, the memory path is sound and the handover ordering above is
the defect; if it still reads poison, the write itself is not reaching the reader, and the record and the
placement are the next suspects. Do not read a `km12to11` pass under `--creator-finish` as a fix: it is the
measurement that says which half to fix.

The pair runs on both D3D11 routes, and that is not redundancy. A keyed mutex orders on the CPU - `AcquireSync`
blocks until the key is released, unlike `ID3D11DeviceContext4::Wait`, which only defers the context's kernel
fence operations - so the CPU-route pair does measure the handover, which a `--sync fence` row on that route
cannot. But a CPU-route opener reads through its own mapping, so a fourth outcome exists there: a read ordered by
the mutex and still stale because that route's read is not ordered against the GPU's write at all. Only the
`.gpu11` pair separates that from the two outcomes above, which is why `km12to11-finish` exists on both arms.

## What refuses

Every refusal below reports `E_OUTOFMEMORY` to the runtime and leaves the device alive. A create's refusal names
its check through `ImportReport::refusal` (`heap-import.h`). An open's writes one
`OpenHeapAndResource: <hr> reported as <admitted>, declined at <step>` line with both blobs' magic and version
beside it. Both go to the debugger channel with no trace build, so a lab log shows them.

The names below are the strings the driver actually writes, so an operator can grep a trial's log for them. The
create's refusals are `ImportReport::refusal` values from `heap-import.cpp`. The open's are the `*why` values of
`open_shared_surface` in `engine-ddi/resources.cpp`. Two rows are `log_line` text from `engine-ddi`, and say so.
An earlier version of this table invented three names (`opened surface records`, `opened surface layout`,
`placed resource on a linear surface`) that appear nowhere in the tree.

| Asked for | Refused by | The name in the log |
|---|---|---|
| a shared buffer, mip chain, array, MSAA, 3D or typeless texture | the envelope, before any callback | the create is never retried. The runtime's own `E_INVALIDARG`, reported as `E_OUTOFMEMORY` |
| a format with no COMPOSED row and no sRGB sibling, including block-compressed and NV12 | the envelope | as above |
| `SHARED_CROSS_ADAPTER` (resource flag 0x4, and `D3D12DDI_HEAP_FLAGS` has no such bit) | `linear_surface_shape` | as above |
| a shared texture on a CPU-visible or system-wide coherent heap, or in the L0 pool | `shared_surface_shape` | as above |
| a shared texture on a `PRIMARY` heap | `shared_surface_shape` | as above |
| a shared heap with no resource, to place resources in later | the shell's allocate | `shareable heap without a resource` |
| a placed resource on an opened or created linear surface | the create, after the base is known | `log_line` "placed: the base is a linear surface, its memory holds that image alone" |
| a retry whose description the engine has no linear image for | attempt 2 | `log_line` "shared surface: the runtime refused the ordinary shape and the engine has no linear image for this description" |
| an open of more than one allocation, or a zero handle | `open_shared_surface` | `arguments` |
| an open whose records the decoder declines | `open_shared_surface` | the decoder's own name for the check that decided: see the table below |
| an open of a format with no COMPOSED row | `open_shared_surface` | `record format` |
| an open whose description the engine has no linear image for | `open_shared_surface` | `engine image` |
| an open whose record and engine layout disagree (pitch, or a backing below what the image needs) | `open_shared_surface` | `layout agreement`, with both numbers |
| an open the shell will not adopt, or whose engine heap or image placement fails | `open_shared_surface` | `adopt`, `engine heap`, `engine image placement` |
| an open naming a protected resource session | `open_heap_and_resource` | `protected resource session` |

### Which check the decoder declined at

One status covered seventeen checks until 2026-10-06, and `declined at record` is not a diagnosis. Round 1 of
BD-075 shipped a D3D12 open that refused every cross-API surface the CPU D3D11 route creates, because that route
writes E26R v2, 16 bytes, while this decoder admits v3, 64 bytes, and reads the DXGI format out of it. The
refusal line already said so - `resource private data 16 bytes (magic 0x52363245, version 2)`, in an 18.8 KB cell
log - and the reason it cost a lab pass is that the kit never surfaced that line, not that the name was missing.
The names below are a convenience that puts the reason first; the information was there. The decoder names its
check (`Bc250SharedSurfaceDecodeWhy`, `BC250_SHARED_SURFACE_WHY_*` in `driver/contract/bc250_shared_surface.h`),
the open's `*why` is that name, and `driver/contract/test/shared-surface-test.cpp` pins every one of them. They
are a contract: the lab kit matches them, so they are not renamed silently.

| The name in the log | What the producer got wrong |
|---|---|
| `record arguments` | a null blob or a null result |
| `record length` | the E26R record is not exactly 64 bytes. **E26R v1 (12 bytes) and v2 (16 bytes) land here**, and that is the whole cross-API failure of round 1 |
| `allocation length` | the LB7A record is not exactly 32 bytes. Kept apart from `record length` because the two blobs come from two producers' code paths, and one name for both would let a regression in either print the other's reason |
| `allocation record` | the LB7A magic or version |
| `record magic`, `record version` | the E26R magic, or a version that is not 3 in a 64-byte record |
| `record policy` | the kernel driver's own parser (`Bc250SurfaceResourcePolicy`) refuses the access bits, so nobody placed this memory the way the record claims |
| `record shared` | the record shares nothing (`Shared != 1`), which the strict D3D12 policy needs |
| `record access` | an access intent the opener does not admit: `PRIMARY`, `CPU_READ` or `SCANOUT` under the strict policy, whose mask is 0 |
| `record geometry` | the width or height is not the allocation's |
| `record extent` | a zero edge, or one over `BC250_SHARED_MAX_EDGE` |
| `record subresources` | more than one mip, slice or sample, or a sample quality |
| `record usage` | a usage that is not DEFAULT, a CPU access flag, or a texture layout of its own |
| `record flags` | a bind or misc flag outside the wire's mask |
| `record format` | no COMPOSED row for the record's DXGI format (reported `E_NOTIMPL`, not malformed) |
| `allocation format` | the LB7A `Format` is not the row the record's DXGI format names |
| `allocation geometry` | the pitch does not hold a row of pixels, or the size does not hold the rows |

The two routes a D3D11 producer may run on are not interchangeable to this opener, and that is measured, not
assumed. The GPU route's shell (`driver/umd/dxvk/ddi-resource.cpp`) writes the full v3 record for every runtime
surface, so a D3D12 open of its shared texture passes every check above. The CPU route's Mesa `d3d10umd` writes
v2, 16 bytes, with no format, no bind flags and `Access = CPU_READ`, so it declines at `record length` and would
decline at `record access` next. A trial that wants a cross-API row to pass must therefore put the D3D11 process
on the GPU route (the application router's allowlist), which is also the route games use.

The same asymmetry runs the other way, and it is not this driver's. A CPU-route **opener** takes the opened
format from the LB7A storage column alone (Mesa `d3d10umd` `OpenResource`) and never reads the E26R record, so a
`B8G8R8A8_UNORM_SRGB` surface opens as `B8G8R8A8_UNORM`: the bytes are right and the view is not. Every content
oracle of `capture-share` compares raw bytes, so that would have passed silently; both openers of the client now
refuse an opened description that is not the creator's, and the lab kit's CPU-route sRGB row expects that refusal
instead of a round trip. The GPU route's shell does carry the record's DXGI format
(`driver/umd/dxvk/ddi-resource.cpp` `decode_open_resource`), so the `.gpu11` row is the one that proves the sRGB
round trip.

One more thing a cross-API row pairs, and the reason the layout agreement in `open_shared_surface` is a real
check and not an assertion: the application router's `GpuUmdPath` may name a D3D11 quartet of its own, with
another RADV build than the installed D3D12 triplet. On the lab it does. A pitch or backing disagreement between
two builds is then a refusal at `layout agreement` that says nothing about a change under test, so the kit prints
the hash of that UMD next to the candidate shell's.

The open's envelope is one rule wider than the create's, on purpose. The decoder bounds an edge at
`BC250_SHARED_MAX_EDGE` (16384), which is what the D3D11 shell creates up to, while a D3D12 shared create stops
at `kLinearMaxEdge` (8192). An opener that refused the larger surface would refuse a surface the other shell
legitimately created. The engine's own image answers for the rest, and the pitch and size agreement still has to
hold. Every other rule is the same, because the decoder admits one mip, one slice, one sample and a COMPOSED
format row only.

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
`NV12`, tier 3 adds `R11G11B10_FLOAT`. The five COMPOSED rows of the surface format table, with the sRGB view of
each 8-bit row, cover six of tier 0's eight - the two `B8G8R8X8` forms have no DXGI entry in our table - and none
of tier 1's or tier 2's. The driver also shares no buffer, and `A8_UNORM` is one row it shares that no tier asks
for. Four of the eight, not six, until the sRGB gap above was closed: this arithmetic became true on 2026-10-06
and was wrong before it.

Tier 2 is a format promise and nothing else. The earlier note here that it "adds keyed-mutex shapes" is not in
the specification: the keyed mutex is the runtime's object at every tier (see "The keyed mutex" above). The
honest answer is this document plus the refusals above: an application that asks for a shape the table does not
hold is told that the driver cannot do it, in a code the runtime survives.

## Order of work

1. Done: refuse without removing the device, name the refusal, and report the open slots as real slots.
2. Done: the measurement that said what a shared create looks like at the DDI.
3. Done: the create and the open, the shared wire format in one header, and the host tests below.
4. Next: the lab trial. `scratch/train/b19-bd075/lab-bd075-real.ps1` (local) runs the shared cells of
   `tools/win/capture-share` in seven small passes, each inside the three-minute bound (the budget gate refuses to
   launch a row whose own bound plus the grace would pass it, so a long pass drops its last row instead). Every shared cell must end in
   `result=pass` with its three content oracles, and the injected negative control must still end in `mismatch`
   with a violated gate and not all three oracles. Both passes take `-Trace`: each row gets its own trace file and
   its own expected number of shared creates and shared opens, and a row whose counts are wrong scores 0 even when
   its pixels are right. The controls `w12` and `f12to12` must show zero of each, which is what says the retry did
   not fire on an ordinary committed texture.
   The cross-API rows need the D3D11 process on the **GPU** route. Its shell writes the full E26R v3 record; the
   CPU route's Mesa `d3d10umd` writes v2, 16 bytes, which this opener declines at `record length` and would
   decline again at `record access`. The trial therefore puts `capshare.exe` and `capshare-peer.exe` on the
   application router's allowlist for the run and restores the list afterwards, and it reads the route each side
   reported back out of the verdict line rather than assuming it. A `--sync fence` row with a CPU-route side is
   not a content measurement at all: that shell copies on the CPU inside `Flush`, so no `Wait` can order the copy,
   and the client reports such a row as `skip`.
5. Later: the shapes the table does not hold (tiled and multi-mip sharing need a deterministic layout, which
   `check_resource_allocation_info` does not implement), cross-adapter sharing, and the reported tier.

## Tests

| Test | What it pins |
|---|---|
| `driver/contract/test/shared-surface-test.cpp` | the two records as byte literals: named checks over the encoder, both decoders, the kernel driver's parser and every single-field refusal. The last of these covers all 5 COMPOSED rows and their 7 format round trips. Since round 2 it also pins the name every refusal reports, one check per `BC250_SHARED_SURFACE_WHY_*` constant, so a log line is a contract and not a courtesy. The runner prints its own total ("67 checks, 0 failures"). Three documents had counted that total by hand, and disagreed |
| `driver/umd/d3d12/allocation-request-test.cpp` | what the create publishes, field by field, and that `prepare_surface` still writes the primary's v1 record |
| `driver/umd/d3d12/heap-import-test.cpp` | the shell half: the shareable envelope, `kShareRequired` only from the runtime's refusal, the shared surface's records and refusals, and the borrowed lifetime of an adopted allocation |
| `engine-ddi/tests/test-shared-create.cpp` | round trip 8 on the GPU: the retry, and the geometry the engine measured. A texel-exact render and read-back of the created surface, in five formats. Both sRGB views are two of the five. Each retry raises the counter once and writes its line once |
| `engine-ddi/tests/test-shared-open.cpp` | round trip 9 on the GPU: one adopt, and a texel-exact clear and read-back of the opened surface. Also both sRGB views, the same handle opened twice, and 12 malformed records. Also an open of two allocations, an open that names a protected resource session, the two Present questions, and the two shells that serve no adopt |
| `engine-ddi/engine-ddi-header-test.cpp` | boundary revision 5, the `AdoptRequest` layout, and `kShareRequired` clamped to `E_OUTOFMEMORY` |
| `tools/win/capture-share/host-validate.ps1` | the real client on a working driver: 35 cells. Among them the two sRGB cells `s12to11-srgb` and `s11to12-srgb`, the keyed handover discriminator `km12to11-finish`, and the four injected negative controls. It proves the oracles before the lab sees them. The route blocker and the opened-format refusal cannot fire on this host - it has one correct driver and no CPU D3D11 UMD - so the blocker is a case of `capshare --self-test` instead |
| `scratch/train/b19-bd075/lab-bd075-classify-test.ps1` (local) | the trial's own scoring: 24 classification cases, 8 restore cases, 11 route-arm cases, 6 allowlist-restore cases, 15 trace cases on written trace files and 6 refusal-text cases. Among the trace cases are the control that shows a retry, the double counting the first rule did, and the 64-byte ICD winsys allocations the first rule counted as shared creates |

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
