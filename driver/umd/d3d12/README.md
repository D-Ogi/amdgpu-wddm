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

`-Arch x86` builds the 32-bit shell for WoW64 processes (the fourth entry of
`UserModeDriverNameWow`) and runs every host gate as an x86 program. Pass
`-MesaSource` with the Mesa tree of the x86 ICD: its `bc250_host_bootstrap.h`
pins the x86 layout of the host contract. `amdgpu_wddm_d3d12.def` keeps the
export name `OpenAdapter12` free of the x86 stdcall decoration, and the recipe
checks the export list, the image machine and that no dynamic C runtime is
imported. The layout gates in `ddi-0092-layout.h` scale with the pointer size.

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
be asked resolves to off and is reported on the log sink (`AMDGPU_WDDM_LOG`,
below). The key is read through
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
fields are not treated as callable slots. A trampoline refuses on its own when a
handle resolves to nothing, when it cannot enter the owner's scope, or when the
call throws; for a slot of the AllowOutOfMemory category (`allow_out_of_memory`,
today `pfnCreateHeapAndResource`, `pfnOpenHeapAndResource`, `pfnCreateStateObject`
and `pfnAddToStateObject`) such a refusal is
clamped to a code the runtime admits, because the runtime cannot tell a
trampoline's refusal from the driver's own and removes the device for any other
one (BD-075).

`DeviceEngineScope` serializes device entry and supplies both runtime callback
authority and hosted instance dispatch on the calling thread. Nested calls for
the same owner are allowed. A different device cannot inherit that authority,
and engine workers do not acquire it merely by holding a pointer. Command-list
entries resolve their owning shell through the engine context.

Deferred command-list replay is on by default and read once per device; the
experiment `deferred-replay-off` (the application profile, the machine-wide value
or, in a lab build only, `AMDGPU_WDDM_D3D12_EXPERIMENT`, with every default and its
off switch in [ddi-trace.h](ddi-trace.h)) takes it back.
On, it uses engine-ddi's deferred command-list replay
([engine-ddi/README.md](engine-ddi/README.md)) with 8 rings of 4 MiB. Its worker
threads are the one kind of engine-ddi thread that enters a device's runtime
domain, together with `HostedDispatch::WorkerScope`: the engine's recording may
allocate, map, lock and wait on the CPU there, while every context, submission,
GPU-side sync and queue operation is refused with the failure note
`replay-worker-op:<op>`. A removal a worker finds marks the device lost at once
and is reported to the runtime from the next drain on a DDI thread. With the
`replay-log` experiment as well, engine-ddi's replay lines (summaries every 10 s,
long waits) also go to `amdgpu_wddm-replay-<image>-<pid>-<device>.log` in the
application profile's `LogDirectory` (REG_SZ next to `Experiment`) or the
temporary directory ([replay-log.h](replay-log.h)): a game started by Steam has no
stderr anyone reads.

Asynchronous pipeline compilation is on by default as well, read once per device.
The experiment `async-pso-off` takes it back, and
[docs/design/per-app-graphics-settings.md](../../../docs/design/per-app-graphics-settings.md)
says how a tester writes it for one game. On, `pfnCreatePipelineState` keeps every
check and every refusal on the calling thread, copies all the description's bytes
that the engine create reads, and returns `S_OK`. The engine's create, which holds
the DXIL to SPIR-V conversion and the shader compile, runs on a process-wide pool
of `logical processors - 1` threads (engine-ddi's `set_pso_policy`,
[engine-ddi/README.md](engine-ddi/README.md)). A consumer of the pipeline waits for
its compile, or makes it on its own thread when no worker has started it, so a
pipeline created and used at once costs what it always did. A compile that fails
after that `S_OK` is logged once with the job's number and the engine's code, and
every use of that pipeline then fails: `SetPipelineState` does not record the bind
and gives the code to the recording command list (`pfnSetCommandListErrorCb`,
which is the device error when no list is entered), and a library store or
serialize of it is refused the same way. The application never draws with a
pipeline the engine refused, and no wrong picture is produced instead. The pool's threads
are the same kind of engine-ddi thread as a replay worker, with the device's
runtime domain and `HostedDispatch::WorkerScope`, because the engine's create
allocates and uploads through the runtime callbacks. The counters of the pool
(creates queued, creates kept synchronous, jobs run by a worker, jobs run by a
consumer, waits with their total and longest time, failures) go to this shell's log
at the device's destruction, every 30 seconds while the device compiles anything,
and from `DllMain` when the DLL is unloaded, because a game can exit without ever
destroying its device. A device that only binds pipelines it compiled long ago
writes no periodic line. Pipelines a game stores into an
`ID3D12PipelineLibrary` are recorded there and handed to the engine's library at
the first `CalcSerializedLibrarySize` or `SerializeLibrary`, so a store right after
a create does not put the serial chain back.

## Queues and fences

`queue-request.h` builds the BC2C v2 context request for the supported single-node
3D, compute and copy queues. A 3D (DIRECT) queue runs on GFX, node 0, and opens its
context when it is created. A compute or copy queue runs where the hosted ICD binds
the engine's VkQueue for it: on the compute ring, node 2 (BC2C COMPUTE), when the
ICD shows a compute queue family, and otherwise on GFX, node 0. Its context is
opened at that bind, inside the queue's creation, with the request of the ICD
when it is one of these two (`QueueEngineRegistry::bind_context`). The ICD hides
the compute family when the kernel driver has no node 2 or when
`RADV_BC250_ACE_QUEUES=0` is set, and the engine then takes the graphics family.
`QueueContext` preserves the full runtime queue handle and callback-owned context.
`QueueEngineRegistry` associates this context with an engine queue; `HostedQueue`
aliases it for RADV submission instead of creating another context for the same
application queue. Internal engine queues use device-owned KT contexts through
`HostedDispatch`.

INLINE execution submits work on the bound context. `fence-ddi.h` copies the
runtime's GPU fence placements into private state without interpreting them as
CPU pointers or inventing KMT handles. The signal DDI selects physical adapter
mask 1 for the runtime to queue its fence operation on that context. It creates
no separate engine fence and writes no completion value on the CPU. Runtime
fence ordering still requires execution validation.

The wait DDI also selects mask 1, and the runtime queues the wait on the queue's
context. A monitored fence belongs to the adapter, so a queue on node 2 waits for
a value that a queue on node 0 signals, and the reverse. Tile mapping and tile-copy
operations go to the engine queue. A failed queue teardown retains ownership and
prevents callbacks through the expired runtime queue handle.

## Heap imports and residency

`RuntimeHeapImports` supplies the engine's `allocate_memory` and `free_memory`
hooks for ordinary buffers. `AllocationRequest` builds a BC2A v2 allocation for
GPU-only VRAM, write-combined GTT or cached GTT, preserving the supplied runtime
resource or heap owner. Size rounding and alignment are checked. Primary,
shared/coherent-systemwide and texture requests outside this buffer path are
refused.

Every refusal taken before anything is allocated names its admission check in
`ImportReport::refusal` and writes one line through `ddi_refusal` (`ddi-trace.h`),
with the request flags, the heap flags, the bits of them this shell has no use
for, and the resource description. A caller reads the same facts from
`last_report()`; the line exists so that a lab log, and `tools/win/capture-share`,
which copies the debugger channel into its cell log, carry them.

## Shared resources

One shape works: a 2D texture of one mip level, one array slice and one sample on
a GPU-only heap, in a format with a COMPOSED row of the surface format table. It
is created as a linear row-major image and published as the same two private-data
records the D3D11 shell reads and writes, so either shell opens what either shell
created. Nothing in the DDI says that a create is shared, so the create learns it
from the runtime's own refusal of the ordinary allocation shape and retries once;
an ordinary create pays no engine query and no retry. The open adopts the
allocation the runtime opened and never deallocates it. `CreateCommittedResource`
with `D3D12_HEAP_FLAG_SHARED`, `ID3D12CompatibilityDevice::CreateSharedResource`
(keyed mutex, whose mutex is the runtime's own object) and
`ID3D12Device::OpenSharedHandle` of a resource all take that path.

Every other shape still fails: buffers, mip chains, arrays, 3D textures, MSAA,
block-compressed and typeless formats, depth-stencil, cross-adapter surfaces,
CPU-visible shared heaps, a shared heap with no resource and a placed resource on
one. They fail with `E_OUTOFMEMORY`, one of the two failures a creation DDI may
report (the other is `D3DDDIERR_DEVICEREMOVED`), so the application loses the
resource and keeps its device - at every layer that can refuse the call, the thunk
included (BD-075; `engine_ddi::admitted_create_failure`,
`native12::ddi_admitted_create_failure`).

Shared fences work in every direction, because the D3D12 DDI has no fence sharing
in it: the kernel object is the runtime's and the driver only consumes two GPU
addresses. What the wire format is, what each half does, what the kernel driver,
the engine and the ICD need (nothing), and why the runtime still reports
`SharedResourceCompatibilityTier` 2, are in
[docs/d3d12-shared-resources.md](../../../docs/d3d12-shared-resources.md).

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

The proof of retirement that reaches the shell is engine-ddi's: the retirement
fences of its engine queues. It cannot cover a submission on a context outside
that set, and trial 245 faulted on a GFX job that was in the ring 2.1 ms before
the unmap of the memory it read (`scratch/m15/game-recon/bsod-245`, local). The
release therefore passes a gate of its own (`ImportReleasePolicy`, M15.8): the
import stays mapped and allocated until the device-wide progress taken when the
release arrived has retired, and until the policy's delay has passed. That
progress is per monitored fence (`device-progress.h`): the ICD signals the
submitting context's fence after every native submit and publishes the value, on
application and internal contexts alike, and `HostedDispatch` sees every such
publication. The delay is a bounded FIFO, oldest first, with a count, a byte and
an age bound; the bounds only shorten the delay and never the progress condition.
A linear primary is never held, because only its own runtime resource may release
it. The device's teardown drains the FIFO in full and counts what was still
unretired there. Both halves of the gate are switchable
(`import-progress-gate-off`, `import-quarantine-off`, and
`release-two-phase-off` for engine-ddi's half; `ddi-trace.h`), all on by default.

Contract sources are the WDK/SDK 10.0.26100 `d3d12umddi.h` callbacks and
`d3dukmdt.h` structures, plus the local Microsoft documentation checkout:
[MakeResidentCb](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb),
[native MakeResident](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_makeresident_cb)
and [native Evict](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_evict_cb).
The engine-side ownership contract is in
[engine-ddi/INTEGRATION.md](engine-ddi/INTEGRATION.md).

## Scan-out primaries (M15.14)

A swap-chain primary can be a scan-out primary: the 64-byte E26R v3 record with
the PRIMARY and SCANOUT bits, which the kernel driver places in the local segment
where the scan-out hardware reads it. The compositor can then flip the game's own buffers
(independent flip) and does not compose them. Trial 478 measured this for The
Witcher 3 at the native mode in exclusive fullscreen and in borderless (M844).

The scan-out primary is on by default. The experiment `scanout-flip-off` turns it
off (`ddi-trace.h`). `scanout-mode.h` holds the decision. For each primary,
`RuntimeHeapImports::scanout_caps_now` reads the kernel driver's scan-out caps
trailer again, and the rule that both application shells share
(`driver/contract/bc250_scanout_primary.h`) compares the chain with the source
mode in that trailer. The rule also reads the compositor's desktop-route record
(`driver/contract/bc250_desktop_route.h`) again for each primary, and stands the
chain down (`desktop-route`) unless the router in `dwm.exe` wrote `gpu` there: the
CPU compositor cannot read a scan-out primary. A game can create its chain before
the mode commit, so the trailer can be one mode behind the chain (session 480).
When the geometry is the only clause that fails, `RuntimeHeapImports::mode_list_now`
reads the kernel driver's mode list for source 0 (`D3DKMTGetDisplayModeList`). If
the list offers the chain's geometry, the chain gets the scan-out primary. The
flip itself waits for the commit: the router's front and the kernel driver compare
each flip with the committed mode. A chain that the rule cannot admit gets the
composed primary, in the shared aperture with its CPU mapping, and never a
failure. Each change of the answer writes one `M15.14 scanout` line to the
debugger channel, with the reason, the chain, the trailer's geometry, the kill
switch, the record (`desktop=gpu`, `desktop=cpu-fallback`, `desktop=absent` and
the other words of `bc250_desktop_route_text`) and the mode list (`modes=not-read`,
`offered`, `not-offered` or `failed`). The process writes at most 64 such lines,
and the last one ends in `budget-spent`.

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
DLLs and a hosted Vulkan instance. Experiments come from the application profile,
the machine-wide value and, in a lab build only, `AMDGPU_WDDM_D3D12_EXPERIMENT`
(BD-167). Such processes have no profile, so they run the defaults unless the
machine-wide value names a switch.
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

None of these lines reach the application's stdio unless something asks
(`stdio-log.h`). `AMDGPU_WDDM_LOG` decides whenever the process carries it:
empty or `0` prints nothing, `stderr` prints to stderr, `file:<path>` appends to
a shared file. The vkd3d-proton engine reads the same sink through engine-ddi,
and so does the DXVK shell. An application that pipes stderr and never reads it
(3DMark's helpers, session 283) blocked once the pipe was full.

Without that variable the control app's timed switch decides: `GameLogUntil` of
`HKLM\SOFTWARE\amdgpu-wddm\Diagnostics` (REG_QWORD, a UTC FILETIME, read from
the 64-bit view so a 32-bit game sees it). While that instant is ahead, the sink
opens a file of its own in `%ProgramData%\amdgpu-wddm\logs\games`
(`<image>-<pid>-<utc>.log`, `CREATE_NEW`, the leaf refused if it turns out to be
a reparse point) and marks the process with two variables:
`AMDGPU_WDDM_LOG=file:<path>`, so that a module loaded later writes into the
same file, and `AMDGPU_WDDM_GAMELOG=<pid>;<the instant as 16 hex digits>`, so
that it takes the bounds with it. A module that finds the marker and its own pid
joins that file under those bounds and opens no second one. A module that finds
another process's pid knows the pair was inherited from the parent that started
this process, ignores both and reads the switch again, into a file of its own.
Without a marker, an `AMDGPU_WDDM_LOG` is the user's own choice and has no
bounds. Every line checks the instant, and the file stops at 64 MiB, measured on
the file so that every module writing into it counts, with one last line saying
so. Each line is formatted first and written with one `WriteFile` on the append
handle, so the lines of several writers of one file arrive whole: a formatted
print into the unbuffered stream reached the file a character at a time and tore
every line of a two-writer probe, at 223 us a line against 2.9 us. A line longer
than the 2 KiB buffer is cut and says `...[line cut]`. The RADV ICD is loaded by
the Vulkan loader and not by this shell. It reads
the same switch and answers the same marker, which is a separate change to the
Mesa fork.

DXGI table publication is not implemented. Present private data size is zero.
The Present handler returns the allocation of one presented surface and the
context of its queue, and refuses every other shape; composition of what it
returns is not validated.

A presentable surface (a linear primary) is allocated with the LB7A v1
description under the E26R v1 resource record. Its storage format is one
that `driver/contract/amdgpu_wddm_surface_format.h` enables for composition
(B8G8R8A8, R8G8B8A8, R10G10B10A2 and R16G16B16A16_FLOAT today); the
compositor converts it to the desktop's format, so the monitor's depth does
not decide it. The same table gives the kernel driver and the compositor's UMD
the format's size a pixel (8 for FP16). The swap chain's colour space and HDR
metadata never reach this DDI: DXGI hands them to the compositor. An FP16
primary is presentable on a machine only when the kernel driver and the
desktop UMD carry the table's FP16 row as well (0.7.184.1, 4176D1DF and
E6B944CF do not); until then the kernel refuses its allocation.
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
