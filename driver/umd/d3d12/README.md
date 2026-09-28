# D3D12 diagnostic adapter

PROVENANCE: Microsoft graphics-driver-samples (MIT), revision de4a2161991eda254013da6c18226f5ea06e4a9c, CosUmd12 adapter negotiation pattern; exact declarations from WDK10.0.26100 d3d12umddi.h.

Build with tools/build/build-umd-d3d12.ps1. It builds a separate amdgpu_wddm_d3d12.dll and runs the export-level adapter tests before reporting success. Existing DLLs are retained by hash. No registration or deployment occurs.

This is the first part of the T0 diagnostic shell. It owns a copy of the adapter callbacks, publishes the eight adapter functions and negotiates0092 as its diagnostic target. It does not claim a functional0092 device: GetCaps now maps the engine ABI1.2 adapter policy; FillDDITable still returns E_NOTIMPL. CalcPrivateDeviceSize and CreateDevice now allocate/construct only the private CPU-side device state for exact R8/build0092; they create no GPU device or queues. Optional table count is0. Unsupported tables are left untouched. Adapter queries call the engine without a GPU device or submission. The module must not be promoted as a working D3D12 driver.

The export test loads the actual DLL and checks null arguments, missing callbacks, count-only version query, insufficient capacity without writes, exact version and adjacent sentinel preservation, unsupported version/table rejection, missing callbacks, device creation/destruction and refusal to close an adapter with a live device. /W4 /WX builds and the tests pass on the host. No lab run.

Next: validate mapped GetCaps through the system runtime and implement the version-specific device/queue/fence tables, create contexts through the D3D12 runtime callbacks using hRTCommandQueue, then run tools/win/d3d12queue under the bounded lab harness. Log stderr is pointer-free and flushed at the adapter boundary. Completion of T0 requires actual runtime queue/fence ordering observations; these host tests do not establish that contract.

## Per-queue runtime context ownership

queue-context.h implements the context lifetime component for the upcoming queue DDI. It copies the create/destroy callbacks and retains the exact hRTCommandQueue for both calls. The caller supplies the node/engine/private-blob request; this component does not infer a KMD contract or share one context across queues. Failed creation leaves it empty; duplicate creation is rejected; failed destruction retains the handle for retry. Successful destruction clears it, and closing an empty context is harmless.

The caller must invoke close from a valid runtime DDI scope and retain the owner if close fails. No destructor calls runtime callbacks, and the component itself provides no locking or cross-thread admission. Those are responsibilities of the device/queue DDI integration, which is still absent. The build now requires mock callback tests for two distinct queues, failure/retry, duplicate creation and missing callbacks. This validates ownership code, not OS acceptance of the context or fence ordering.


Device state owns a copy of the0062 user-mode callback table and the runtime device handle. The adapter live-device count prevents premature adapter deletion. Runtime-owned private storage is constructed in place and is not freed by the UMD. The version check precedes callback-table access. Logs retain Interface, Version and Flags separately. Exact0092 is the diagnostic target selected from offline analysis of the lab System32 runtime: its FillAPIVersions table contains52 records and ends at R8/build92 (M758). Live negotiation remains unmeasured. GetCaps/FillDDITable still refuse use, so this is not a functional D3D12 device. See evidence/windows/2026-09-28-E34-m15-runtime001.

## KMD context request

queue-request.h constructs the BC2C v2 context blob using driver/contract/bc250_umd_submit.h, so its compile-time layout checks apply here too. The request owns the blob and cannot be copied with a stale pPrivateDriverData pointer. It accepts ordinary3D/compute/copy flag combinations on physical node0 (NodeMask0 or1), routes them to the GFX IP, sets EngineAffinity1, and rejects paging/video, other node masks, scheduling groups and creation options not implemented by this path. Rejection clears the request, preventing reuse of an earlier successful blob.

Mandatory queue-request-test checks the complete blob at the runtime callback boundary, all reserved fields, hardware-queue flags and seven accepted flag combinations, then validates refusal of unsupported nodes/queue classes. The test uses the same QueueContext owner as the forthcoming DDI queue implementation. No real runtime/KMD context has been created by these tests; the public FillDDITable gate remains closed until the required table entries are implemented.

## Queue ownership beyond runtime private storage

QueueRegistry combines request construction and per-queue context ownership. QueueSlot holds only a pointer; the owner resides on the heap. Failed creation publishes no slot. Destroy clears the slot on success and failure; on failure it retains a retired record, invalidates the queue runtime handle and callback pointers, and the typed DDI reports device removal.

No callback retry occurs after DestroyCommandQueue returns: that would require an unproved guarantee about hRTCommandQueue lifetime. discard_retired_metadata refuses active queues; otherwise it frees only CPU records and returns S_FALSE plus the unresolved context count. This is explicitly not kernel cleanup. DestroyDevice now detaches and frees all remaining CPU queue records after queue DDI calls have stopped, logging retired and unexpectedly active counts separately. It calls no queue callback and touches no runtime queue storage. Lab object-census evidence is still required before claiming contexts are reclaimed by OS teardown.

Same-queue lifetime and device shutdown calls must be serialized by the DDI integration. Different-queue list changes use an SRW lock; callbacks execute without that lock. Tests cover two contexts, creation/destruction failures, slot release and metadata disposal with no callback using the retired handle.

## Typed queue DDI functions

queue-ddi.h installs CalcPrivateCommandQueueSize, CreateCommandQueue and DestroyCommandQueue into a private core0088 table, the core layout retained by0092. The adapter and functions share device-state.h. Successful create initializes runtime storage before publishing ownership; a failed callback publishes no context. Destroy reports D3DDDIERR_DEVICEREMOVED through SetErrorCb on failure and sets sticky device loss; later create/size requests are rejected. Retired context ownership stays in the registry.

The DLL export test now creates a device through OpenAdapter12/CreateDevice and exercises these typed queue functions against that actual private state, then destroys queue/device/adapter. The separate typed-table test covers two runtime queue identities, uninitialized storage, OOM, failed destruction, sticky loss and retired metadata disposal. All build gates pass /W4 /WX. This is still host callback simulation: FillDDITable does not publish the incomplete table, and kernel context reclamation after failed queue destruction remains a lab promotion blocker. The export test now injects a failed destroy and verifies that actual DLL device teardown does not retry the expired runtime handle. Registry tests also cover terminal disposal of mixed active/retired records and repeated empty disposal.

## Fence placement lifetime

fence-ddi.h provides typed CalcPrivateFenceSize/CreateFence/DestroyFence for the single-adapter path. It copies both GPU placements and flags into runtime-owned private storage; it never retains the caller's Fences array, dereferences a GPU address on the CPU, or invents an importable KMT handle. Linked-adapter arrays and unknown flags are rejected. Device loss rejects new fence state. Destroy releases only the UMD metadata.

The mandatory host test overwrites the original argument after creation, verifies both retained placements, adjacent storage sentinels, failure without writes, unsupported linked-adapter requests and device-loss rejection. These functions are installed only in a private test table. They do not signal, wait, submit GPU work or establish the runtime's ownership of queue fence operations. T0 still requires live system-runtime measurements before publishing the complete DDI table.

## Adapter contract admission

OpenAdapter12 queries the runtime's exact adapter before allocating or publishing an adapter handle. It requests a zero-initialized v3 caps buffer plus the B2AI identity trailer, using the shared contract headers and their layout assertions. Admission requires the v3 magic/version/size, measured caps, a populated GFX ring and submittable node0, plus the identity trailer's magic/version/size and reserved field. The complete caps and OS-assigned LUID are retained in the adapter; no LUID or pointer is printed. Callback errors propagate unchanged and incompatible blobs return E_NOINTERFACE with empty output handle/table.

The actual-DLL test covers callback failure, foreign magic, old version, wrong size, absent submission/GFX/ring support, unmeasured fields, missing/invalid identity and the positive full-width LUID. Query storage is checked for zero initialization, so a legacy KMD writing only the prefix cannot accidentally supply a stale trailer. This validates the boundary with mock responses; live KMD171 admission and native device negotiation still require the bounded runtime probe. These hardware declarations are not a substitute for measured D3D12 feature-level capability admission.

adapter-kmt-probe.exe is a read-only lab admission check. It enumerates the BC-250 through DXGI, opens its KMT adapter, bridges QueryAdapterInfo to KMTQAITYPE_UMDRIVERPRIVATE, loads an explicitly named diagnostic UMD, and calls OpenAdapter12/CloseAdapter. It independently compares the returned identity with DXGI without printing the LUID, then closes the KMT adapter. It creates no device/context or GPU submission. Run only under the bounded lab supervisor with exact artifact hashes. Passing proves KMD-backed admission, not D3D12 runtime negotiation or rendering.

## DDI0092 layout and request diagnostics

The DLL build includes ddi-0092-layout.h: x64 static assertions for adapter, core0088, command-list0092, queue0001, extended0021 and callback0062 tables, plus the selected options/shader/architecture payloads. Compilation with WDK10.0.26100 validates the expected sizes. DXGI table selection is deliberately not inferred from byte size; it still needs negotiated interface handling.

GetCaps logs type, DataSize and whether pInfo exists; FillDDITable logs type, size, table number and whether hRTTable exists. Neither prints pointers. They continue returning E_NOTIMPL while the implementation is incomplete. These diagnostics and layout checks do not establish runtime call order or prove that queue Signal/Wait is handled by the runtime. Older capability payload sizes must be mapped with verified layouts before they are accepted.

## Runtime memory request and ownership

`allocation-request.h` owns the BC2A v2 blob and the D3D12 Allocate_0022
request for one ordinary raw-memory allocation. It preserves the supplied
runtime owner (including a heap owner), rejects zero/overflowing sizes and
invalid alignments, and selects explicit GPU-only VRAM, CPU write-combined GTT
or CPU-cached GTT policy. It does not infer a texture layout, sharing, sparse
backing, residency or an exact GPU VA. Those require further integration.

The mandatory request test links the actual KMD `umd_blob.c` parser and checks
all three policies, size rounding, malformed requests and sparse refusal.
`allocation.h` retains the returned allocation handle after failed release and
forbids callbacks after runtime invalidation. Deallocation names only that
allocation through HandleList, never simultaneous resource-group destruction.
An allocation's reported address may remain zero until an explicit mapping is
completed; it is not yet an import-ready GPU VA.

The host gates validate request shape and ownership, not native GPU allocation
or rendering. The shell still needs the allocation registry, completed VA and
residency operations, GPU-use retirement and same-storage import on the engine's
VkDevice before these requests are used by native CreateHeapAndResource.

## Paging callback lifetime and pending mappings

CreateDevice now copies the kernel-thunk callback table into its device state.
The actual-DLL gate clears the caller's table after creation and checks the
retained function pointer. `paging.h` uses those callbacks with hRTDevice to
own a paging queue and ordinary writable GPU VA mappings. It exposes an address
only after the mapping's pending paging fence completes. S_OK mapping results
do not inherit an undefined asynchronous fence value. The UINT64_MAX fence
sentinel rejects use after device loss.

Mapping ownership is tied to its paging domain; a different device/domain
cannot free it. Failed unmap preserves the mapping and blocks queue destruction.
Invalidating runtime scope clears callbacks and the borrowed CPU fence pointer.
No destructor retries callbacks. A successful but malformed mapping stays owned
and unusable, requiring explicit terminal recovery rather than fabricated
completion. Calls and device teardown must be serialized by integration.

The caller must separately establish residency and GPU-use retirement before
unmapping; completion of a paging operation is not completion of rendering.
The mandatory host tests cover pending/ready state, device mismatch, retained
ownership after failures, synchronous mapping, loss and expired callback scope.
Native device creation and same-storage Vulkan import remain unverified.

## Native residency callbacks

`residency.h` calls the D3D12 core-layer MakeResident/Evict callbacks with the
runtime device and runtime paging-queue handles. It sends the complete translated
allocation list in one callback, without splitting, retries or deduplication of
residency references. This avoids partial success across multiple batches.
Allocation ownership and object-to-allocation translation remain the registry's
responsibility; this helper does not accept arbitrary engine allocations as proof
of device ownership.

S_OK means ready. E_PENDING remains E_PENDING and carries the runtime queue's
paging fence; it must not be compared against `PagingDomain`'s separate KT fence.
E_OUTOFMEMORY preserves the trim amount and ignores the fence output. The helper
does not trim app resources or retry the failed operation. Malformed successful
or pending replies produce Unknown state and retain the original callback status
for diagnosis; integration must stop and resolve ownership rather than reissue
an operation that may already have incremented residency references.

Eviction is explicit, after the caller retires GPU uses, and propagates callback
failure. Runtime invalidation prevents further callbacks; destruction makes no
callback. The mandatory host gate covers pending vs ready, the full-width opaque
runtime queue, one-call batching, OOM atomic refusal, failure propagation,
malformed outputs, repeated residency requests and callback lifetime.

Contract sources: WDK/SDK 10.0.26100 `d3d12umddi.h` callback declarations and
`d3dukmdt.h` residency structures; Microsoft DDI references
[MakeResidentCb](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb)
(the DirectX 12 remarks describe atomic OOM refusal and split-batch rollback),
[native MakeResident callback](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_makeresident_cb)
and [native Evict callback](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/nc-d3d12umddi-pfnd3d12ddi_evict_cb).
These were read from the local Microsoft documentation checkout. The callback
bridge is host-tested; heap registry integration, runtime fence scheduling and
native GPU execution are not established by this gate.

## Ordering pending mappings on a GPU context

`PagingDomain::address_for_context` obtains a mapping address for subsequent
submissions on one context. If paging has not completed, it queues
`pfnWaitForSynchronizationObjectFromGpuCb` using that context and the paging
queue's monitored fence. It makes no CPU wait. A failed callback returns no
address; a device-loss sentinel observed during the call also refuses use.
The context must belong to this device and remain serialized/lifetime-valid
under the caller's queue registry. The method preserves the full HANDLE.

This does not change `ready`: a CPU observer still sees E_PENDING until the
paging fence actually advances. Another context needs its own ordering, and
residency must be established separately. A queued wait is not evidence of GPU
retirement and does not authorize unmapping or freeing the allocation. The
current engine import boundary still requires a completed mapping; this helper
does not silently relax that contract.

The host gate leaves the fake paging fence unchanged while queuing waits on two
distinct contexts, verifies the fence handle/value and full-width context,
checks failure/loss propagation with empty output, and confirms that completed
mappings need no additional wait. No native GPU execution is claimed.

## Device memory registry

`MemoryRegistry` ties runtime allocation ownership to a paging mapping. Records
are allocated and linked before the first runtime callback; no tracking allocation
is needed after the runtime has created a GPU allocation. Keys contain the registry
identity and a monotonically increasing serial, so another device or a stale key
cannot resolve a new allocation that reused the same CPU address.

Create accepts allocation plus mapping and may leave paging pending. Snapshot
returns an allocation/address only after mapping completion. GPU residency and
Vulkan import are separate integration requirements. Once release starts, snapshots
are refused. Unmap precedes runtime deallocation; failures retain the record and
successful steps are not repeated on a later retry in a still-valid runtime scope.
Call `invalidate_owner` before that scope expires. A failed create cleans up before
returning, or retains an inaccessible retired record if cleanup also fails.

Runtime callbacks execute outside the registry lock. Same-record operations are
pinned against simultaneous cleanup, but integration must still serialize operations
on the shared PagingDomain as its contract requires. Device teardown runs only after
DDI callers stop, discards CPU metadata without callbacks, and reports unresolved
active/retired memory counts. This is not a claim of OS reclamation.

The adapter026 host build covers successful allocation/mapping/release, pending
mapping completion, stale/foreign keys, reentrant registry inspection from callbacks,
failed unmap/deallocation and retry, create rollback, failed rollback and expired
owners. The registry is stored in native device state; heap-slot/Vulkan import wiring
and native runtime execution remain incomplete.

## Adapter policy integration

The build now links the native engine-ddi library after its pinned-header,
/analyze, native-policy and caps gates pass. The recipe runs that sub-build
under PowerShell7 so expected diagnostic stderr from a negative test is not a
PowerShell5 NativeCommandError. No harness implementation enters the UMD.

On the first GetCaps, adapter-caps.cpp loads amdgpu_wddm_vkd3d.dll and
amdgpu_wddm_radv.dll from the UMD's own directory, with only that directory and
System32 searched for dependencies. A serialized, one-time QueryAdapterCaps
batch produces the immutable adapter snapshot. It is freed at CloseAdapter
before either DLL reference is released. Failures are returned and retained
for that adapter lifetime; no invented capability fallback is used.

The query scope injects bound queues and the adapter-only RADV extension while
preserving the engine's instance requirements. It rejects device creation and
unexpected callbacks. M769 measured this physical-query path on unit A, with
FL11_1/tiled0/binding3/RT1.1 engine answers. These are not native UMD support:
the mapper suppresses features whose DDI slots are still unimplemented.
Adapter031 builds and all host gates pass. A fresh system-runtime test of the
mapped GetCaps remains necessary; device construction and rendering are open.
