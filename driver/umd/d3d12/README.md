# Native D3D12 UMD

PROVENANCE: Microsoft graphics-driver-samples (MIT), revision de4a2161991eda254013da6c18226f5ea06e4a9c, CosUmd12 adapter negotiation pattern; exact declarations from WDK10.0.26100 d3d12umddi.h.

This UMD targets interface R8/build 0092. Its current implementation connects
system D3D12 runtime callbacks to an inline vkd3d-proton engine and hosted RADV.
The immediate target is a native buffer copy with upload, GPU execution, fence
completion and readback. This document describes the code and its host checks;
it does not establish successful public device creation, rendering or game support.

Build with `tools/build/build-umd-d3d12.ps1`. The recipe builds
`amdgpu_wddm_d3d12.dll`, runs its host gates and retains existing DLLs by hash.
It performs no registration or deployment. The engine ABI and library are pinned
by [engine-ddi/engine-abi.json](engine-ddi/engine-abi.json); the engine sub-build
checks those pins and runs under PowerShell 7. Only production engine code is
linked into the UMD.

## Adapter and device ownership

`OpenAdapter12` admits the runtime's adapter through the v3 caps blob and B2AI
identity trailer. `adapter-contract.h` checks the submission requirements and
retains the OS LUID. An incompatible blob returns `E_NOINTERFACE` without
publishing an adapter handle or table. These hardware declarations do not imply
a D3D12 feature level.

`adapter-caps.cpp` loads `amdgpu_wddm_vkd3d.dll` and `amdgpu_wddm_radv.dll` from
the UMD directory, restricting dependency search to that directory and System32.
A serialized `QueryAdapterCaps` batch caches the engine's policy for the adapter
lifetime. The query bootstrap permits adapter enumeration and refuses device
creation or unexpected runtime work. `GetCaps` maps this snapshot through the
engine boundary, suppressing features whose DDI support is absent.

`CreateDevice` copies the runtime callback tables and constructs a real engine
device in INLINE mode with a private Vulkan instance. The hosted bootstrap
preserves the engine's instance chain, supplies a device identity and checks that
`GetVulkanHandles` returns that same instance. It then installs a runtime-backed
engine context, heap import hooks and queue ownership.

Device destruction releases the engine context, imports and engine while their
runtime and instance scopes remain valid. Failed unbind, unresolved objects or
an outstanding instance retain the owner and pin its code. The adapter retains
its dependencies while such owners exist. Terminal metadata disposal does not
claim kernel object reclamation, and destructors never retry expired callbacks.

## Published tables and entry scopes

`native-tables.cpp` implements `FillDDITable` for these exact layouts:

| Table | Layout | Publication |
| --- | --- | --- |
| Device core | `D3D12DDI_DEVICE_FUNCS_CORE_0088` | Engine functions plus typed shell entries |
| Command lists | `D3D12DDI_COMMAND_LIST_FUNCS_3D_0092` | Separate compute and graphics indices, with retained runtime table handles |
| Command queue | `D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001` | Execution, signal selection and explicit unsupported-operation handlers |
| Extended features | `D3D12DDI_EXTENDED_FEATURES_FUNCS_0021` | Empty supported-feature list; feature enabling is unsupported |

Composition validates the requested size and index before publishing the table.
`ddi-0092-layout.h` checks the WDK layouts. `ddi-entry.h` provides exact typed
trampolines for the 122 core slots and 70 slots in each command-list table;
original functions are immutable after publication, and refills must match.
Queue and extended-feature calls use the same entry policy. Reserved queue
fields are not treated as callable slots.

`DeviceEngineScope` serializes device entry and supplies both runtime callback
authority and hosted instance dispatch on the calling thread. Nested calls for
the same owner are allowed. A different device cannot inherit that authority,
and engine workers do not acquire it merely by holding a pointer. Command-list
entries resolve their owning shell through the engine context.

## Queues and fences

`queue-request.h` builds the BC2C v2 context request for the supported single-node
3D, compute and copy queues. `QueueContext` preserves the full runtime queue
handle and callback-owned context. `QueueEngineRegistry` associates this context
with an engine queue; `HostedQueue` aliases it for RADV submission instead of
creating another context for the same application queue. Internal engine queues
use device-owned KT contexts through `HostedDispatch`.

INLINE execution submits work on the bound context. `fence-ddi.h` copies the
runtime's GPU fence placements into private state without interpreting them as
CPU pointers or inventing KMT handles. The signal DDI selects physical adapter
mask 1 for the runtime to queue its fence operation on that context. It creates
no separate engine fence and writes no completion value on the CPU. Runtime
fence ordering still requires execution validation.

Queue `Wait`, tile mapping and tile-copy operations are unsupported and report
failure through the shell. A failed queue teardown retains ownership and prevents
callbacks through the expired runtime queue handle.

## Heap imports and residency

`RuntimeHeapImports` supplies the engine's `allocate_memory` and `free_memory`
hooks for ordinary buffers. `AllocationRequest` builds a BC2A v2 allocation for
GPU-only VRAM, write-combined GTT or cached GTT, preserving the supplied runtime
resource or heap owner. Size rounding and alignment are checked. Primary,
shared/coherent-systemwide and texture requests outside this buffer path are
refused.

A separate KT paging queue maps the allocation. The importer waits at most two
seconds for a pending map and requires a completed, aligned GPU VA before
returning memory. An incomplete map or failed cleanup remains owned. The private
`bc250_host_import` identifies the same runtime allocation and device identity on
the engine's `VkDevice`. Allocation uses `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`
and no dedicated-allocation structure. Memory types are checked against buffer
requirements and the requested CPU access policy. Borrowed `Lock2`/`Unlock2`
operations route back to the heap owner for upload and readback mappings.

Heap creation does not call `MakeResident`. `native-residency-ddi.cpp` resolves
engine objects to runtime allocation handles, removes aliases within the request
and sends one complete list through the native runtime residency callbacks.
`E_PENDING` preserves the runtime paging fence and sets `WaitMask`; that fence
is distinct from the importer's KT mapping fence. Empty validated allocation
lists require no kernel operation. Unknown ownership or malformed callback
results fail rather than manufacturing residency.

After engine GPU-use retirement, release frees the Vulkan import, unmaps its VA
and deallocates the runtime allocation in that order. Failed steps retain their
records. Mapping completion alone is not GPU-use retirement. `PagingDomain` also
provides a context-specific GPU wait helper, but imports still require a completed
mapping before publication.

Contract sources are the WDK/SDK 10.0.26100 `d3d12umddi.h` callbacks and
`d3dukmdt.h` structures, plus the local Microsoft documentation checkout:
[MakeResidentCb](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb),
[native MakeResident](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_makeresident_cb)
and [native Evict](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_evict_cb).
The engine-side ownership contract is in
[engine-ddi/INTEGRATION.md](engine-ddi/INTEGRATION.md).

## Diagnostics and remaining boundaries

Set `AMDGPU_WDDM_DDI_TRACE=1` before process startup to enable paired DDI entry and
exit records. The switch is sampled once. Records contain the slot name,
sequence, QPC, thread and outcome. Optional typed observations record format,
sample count, flags and capability outputs. Private pointers, resource data and
opaque handles are excluded. Entry is recorded before owner resolution so a rejected scope can
still be identified. A normal return from a void DDI is not proof that its work
succeeded; error callbacks and GPU completion must be examined separately.
Adapter negotiation, engine startup, hosted callbacks and teardown have their
own diagnostics.

DXGI table publication is not implemented. Present private data size is zero.
The Present handler returns the allocation of one presented surface and the
context of its queue, and refuses every other shape; composition of what it
returns is not validated.
Queue Wait, tiled resources, scheduling groups, offer/reclaim and background
processing are also outside the current shell. A non-null function pointer may
be an explicit refusal handler, so table publication is not a capability claim.
See [engine-ddi/SLOTS.md](engine-ddi/SLOTS.md) for the engine boundary's slot map.

## Host validation

The build runs tests for adapter admission and export negotiation, typed table
composition, entry scopes and diagnostics, queue/context ownership, hosted
callbacks, fence placement, allocation requests, paging, heap import and native
residency. Tests use fake callbacks or engine test objects to exercise successful
paths, failures, retained ownership and teardown. Allocation-request tests also
use the actual KMD blob parser. Compilation uses `/W4 /WX`, with `/analyze` on
the selected ownership and integration gates.

The export test checks the built DLL without creating a GPU device on the
development PC. `adapter-kmt-probe.exe` separately supports read-only KMT adapter
admission and identity comparison without device creation or submission. Neither
these checks nor table layout assertions replace a system-runtime buffer-copy
and readback test.
