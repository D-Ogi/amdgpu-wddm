# D3D12 diagnostic adapter

PROVENANCE: Microsoft graphics-driver-samples (MIT), revision de4a2161991eda254013da6c18226f5ea06e4a9c, CosUmd12 adapter negotiation pattern; exact declarations from WDK10.0.26100 d3d12umddi.h.

Build with tools/build/build-umd-d3d12.ps1. It builds a separate amdgpu_wddm_d3d12.dll and runs the export-level adapter tests before reporting success. Existing DLLs are retained by hash. No registration or deployment occurs.

This is the first part of the T0 diagnostic shell. It owns a copy of the adapter callbacks, publishes the eight adapter functions and negotiates0092 as its diagnostic target. It does not claim a functional0092 device: GetCaps and FillDDITable return E_NOTIMPL. CalcPrivateDeviceSize and CreateDevice now allocate/construct only the private CPU-side device state for exact R8/build0092; they create no GPU device or queues. Optional table count is0. Unsupported tables are left untouched. There are no engine calls or GPU submissions. The module must not be promoted as a working D3D12 driver.

The export test loads the actual DLL and checks null arguments, missing callbacks, count-only version query, insufficient capacity without writes, exact version and adjacent sentinel preservation, unsupported version/table rejection, missing callbacks, device creation/destruction and refusal to close an adapter with a live device. /W4 /WX builds and the tests pass on the host. No lab run.

Next: implement measured caps and the version-specific device/queue/fence tables, create contexts through the D3D12 runtime callbacks using hRTCommandQueue, then run tools/win/d3d12queue under the bounded lab harness. Log stderr is pointer-free and flushed at the adapter boundary. Completion of T0 requires actual runtime queue/fence ordering observations; these host tests do not establish that contract.

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
