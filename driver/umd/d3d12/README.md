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

Before that query the shell resolves the adapter's instance policy once and
chains it, as `bc250_host_policy`, into the query instance and into every device
instance, so both are given the same answer. The policy decides on sparse
binding in the hosted ICD, which the reported tiled resources tier and feature
level 12_0 and above depend on; the process environment is not asked. Sparse
binding is on when the DWORD `AmdgpuWddmSparseBinding` in the adapter's
software key is 1 or the system names that value as not found, and off when it
is 0. Any other value, an unspecified failure of the query or a key that cannot
be asked resolves to off and is reported on stderr. The key is read through
`QueryAdapterInfo`, so a later edit reaches neither a cached adapter nor its
devices. The guarantee holds for an ICD that recognizes the structure: an older
one ignores it and keeps its environment behaviour, so reported features alone
do not show that the policy was read. The default is a lab default: its cost, a null-PRT load fixup in
every pipeline and one more queue context per device, is not measured yet.

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

## Registration between sessions

The UMD is the fourth `UserModeDriverName` entry, and it loads the engine and
the ICD from its own directory: the three files are one unit, the triplet.
dxgkrnl reads the names at adapter start, while each process loads the files
when it opens the adapter, so new contents at the same path reach processes
started afterwards without an adapter restart (M765-M768). Between sessions the
directory holds either the diagnostic shell alone, which has no sibling loader
and answers device creation with `DXGI_ERROR_UNSUPPORTED` (M768), or an accepted
triplet. With an accepted triplet every process that creates a D3D12 device on
the adapter, for example Task Manager's DirectX version probe, loads all three
DLLs and a hosted Vulkan instance. Experiments (`AMDGPU_WDDM_D3D12_EXPERIMENT`)
come from the process environment only, so such processes run the defaults.
The sparse policy stays at its default (value absent, on); the DWORD set to 0
removes FL 12_x from processes started afterwards without removing D3D12.
A file that a running process maps cannot be deleted or overwritten in place,
only renamed: replacements use `ReplaceFile` with a named backup, and the kill
switch is the diagnostic shell put back at the registered path that way.
Running processes keep what they loaded.

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

A presentable surface (a linear primary) is allocated with the LB7A v1
description under the E26R v1 resource record. Its storage format is one
that `driver/contract/amdgpu_wddm_surface_format.h` enables for composition
(B8G8R8A8, R8G8B8A8, R10G10B10A2 today); the compositor converts it to the
desktop's format, so the monitor's depth does not decide it. The same table
gives the kernel driver and the compositor's UMD the format's size a pixel.
It is released by its runtime
resource, with ASSUME_NOT_IN_USE and SYNCHRONOUS_DESTROY, and only inside the
`pfnCreateHeapAndResource` or `pfnDestroyHeapAndResource` call of that
resource (the owner scope in `heap-import.h`). The destroy waits up to two
seconds for the work submitted before it. A release that reaches the shell
after that call has returned frees the Vulkan import and the mapping, makes
no runtime callback in either form, keeps the allocation in its record and
reports the device error. Other imported heap memory is released by handle
list with the same two flags, after the engine has retired its uses. The
hosted driver's own allocations are internal ones and keep flags NONE.

The hosted driver reserves address ranges, maps zero pages and allocations
inside them and queues address updates through `HostedDispatch`
(`hosted-sparse-test.cpp`). An extent has one owner, an allocation's ordinary
mapping or a reservation, and a runtime answer that would give it two removes
the device. What is mapped inside a reservation is not recorded. One update
names one reservation; its backing, range, fence and context are held until
the runtime callback returns, the imported heaps among them. The queue's
`pfnUpdateTileMappings` and `pfnCopyTileMappings` are one operation of the
queue, admitted as an execute is, and hand their arguments unchanged to the
engine queue. Both are host and harness tested; no run against the system
runtime has used them.
Queue Wait, scheduling groups, offer/reclaim and background
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
