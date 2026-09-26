# Hosted runtime allocation and submission control

Hypothesis: RADV can allocate memory, submit a GPU fill and observe its fence
through callbacks belonging to the native D3D device during CreateDevice.

This extends E34 M536 with private ABI version 2. Apply icd.patch and umd.patch
to the source states identified by source-manifest.json. The manifest binds both
sides of every changed file. Existing E35 pipeline and quality changes remain.
PROVENANCE: Mesa MIT; Microsoft WDK/SDK 10.0.26100 declarations.

The bridge converts KMT request fields into runtime callback structures. Context
handles stay in a per-device table as HANDLE values; winsys receives table tokens.
An unsupported operation fails without calling a KMT fallback. Standalone Vulkan
retains its original dispatch. Hosted fence status relies on callback errors;
there is no runtime callback equivalent of the standalone GetDeviceState query.

Scope is a synchronous CreateDevice spike. Its stack-owned descriptor and strict
entry-thread check are valid only until this control destroys its VkDevice and
VkInstance. It is not a persistent rendering screen. External sharing, sparse
updates, hardware queues and worker-thread callbacks remain unsupported.

Build the existing E34 UMD recipe and the RADV WDDM recipe. Run the fast quality
gates before staging. Preserve previous binaries and check the exact staged
hashes. Use the bounded E34 process router, with CPU UMD and ICD restoration in
finally, STOP clear and temperature below 85 C. Do not restart DWM or the OS.

The native two-device control creates each hosted VkDevice on queue family 0.
It allocates a 16 KiB host-visible buffer, submits vkCmdFillBuffer with pattern
0x39c57a16, inserts a transfer-to-host barrier, waits up to 10 seconds for its
fence, invalidates the mapped range and compares all 4096 words. Record callback
HRESULTs, process exit, artifact hashes and restoration checks. The outer process
deadline is 45 seconds. Existing standalone pixel checks remain separate.

A pass requires two hosted content passes, successful teardown, no wrong-thread
callbacks and the unchanged standalone two-device control. A callback failure,
timeout or mismatch rejects the candidate. No result from this control establishes
GPU DWM, native Present ordering or absence of full-frame CPU copies.

Local build: both DLLs link; all eight fast gates pass. Bounded runtime control passed as M539.

## Persistent screen control

Hypothesis: a private hosted Zink screen can remain attached to its D3D device
across DDI entries and render the existing two-device exact-color control.
Wrap D3D and DXGI device entrypoints in a thread-local runtime scope. Reject
callbacks from workers or another device. For this bring-up only, disable
threaded submission and run Zink program jobs synchronously in that scope.

Use BC250_HOSTED_RENDER=1 starting with candidate003 and run009. Retain the 45-second
outer deadline and baseline restoration. Require all three 4096-pixel checks,
correct surviving-device lifetime and no wrong-thread callback. This is offscreen;
Present, sharing and DWM remain separate gates.

## Borrowed runtime surface control

Hypothesis: hosted Zink renders directly into an allocation created for a native
D3D shared texture. The private import carries the device identity, allocation
handle, mapped GPU VA and size. The UMD owns residency, VA and deallocation;
RADV borrows them and never frees the allocation. No surface Lock2 is used.

Candidate007/run014 changes the existing color control to use shared render
targets, retaining a separate staging resource as the readback oracle. Require
three exact 4096-pixel colors including the surviving device. The import uses
explicit linear layout and the runtime surface pitch. Teardown waits for GPU
completion before releasing the imported Gallium resource and runtime allocation.
This control does not exercise Present or DWM.

## Borrowed residency correction

Apply borrowed-residency.patch after the runtime-import patches, checking its
before/after manifest. It excludes borrowed BOs from RADV destruction-time Evict;
UMD residency ownership is unchanged. Re-run the unchanged shared-color control
with all three pixel checks and successful independent baseline restoration.
M542 records run019 and the scoped build gates; Present remains a separate gate.

## GPU ordering before native Present

Hypothesis: each hosted RADV queue can publish its accepted GPU progress signal
through private ABI 4; UMD can enqueue waits for all active queue witnesses into
the runtime Present context without a CPU render-fence wait. Runtime imports
receive a Zink batch-end release barrier to GENERAL and FOREIGN, and no exported
Vulkan semaphore. Later use reacquires through Zink's existing image barriers.

Build both DLLs and pass fast gates. First rerun the unchanged shared-color
control (run020). Then run runtime-present-control in the interactive lab session:
320x240 native BGRA window, 120 Presents at 100 ms intervals, red/blue/green
phases and an end-only staging readback. Capture composed window pixels during
presentation. Require Present success, GPU-wait witnesses, exact final pixels,
successful teardown and hash-checked baseline restoration. A black window,
failed callback, reset, timeout or mismatch rejects the candidate. Record DWM
identity; it remains the CPU control for this client test. No G0 inference.

Reference: WDK 10.0.26100 D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU and
pfnWaitForSynchronizationObjectFromGpuCb, workspace ref/ddi-display/d3dumddi.md.

Ordering refinement for run026: keep the runtime Present context distinct from
the RADV graphics context. After PresentCb, signal a UMD-owned monitored fence;
before subsequent SubmitCommand callbacks, enqueue waits for its latest value
in each render context (once per value/context). Resource teardown waits for
Present completion with a 10-second asynchronous CPU-wait deadline. CPU waits
are teardown-only. Direct Present in the RADV context was rejected in run025;
its failure is retained, not counted as acceptance. A three-capture requirement
is evaluated after process output is preserved, so early failure cannot hide
the original exit/error. Candidate014 is the bidirectional version.

Apply present-icd.patch and present-umd.patch after borrowed-residency.patch.
For exact replay normalize the verified input to LF and use git with
core.autocrlf=false. present-manifest.json binds original and LF input hashes
and final byte hashes. M543 records run026, candidate014, and the unsuccessful
intermediate controls. The full project patch stack still needs a clean build.

## Hosted loss propagation control

ABI5 adds sticky device status and loss notification to the hosted bridge.
HRESULT removal/reset/hung results retain device-loss identity; invalid fence
observations cannot satisfy waits. UINT64_MAX is reserved by this private
hosted timeline contract. The Microsoft monitored-fence documentation does not
establish that every TDR writes this value, so no such universal claim is made.

First run the existing120-Present positive control. Then use an app-scoped
BC250_HOST_TEST_LOSS=submit or fence diagnostic, also requiring the existing
BC250_D3D_RUNTIME_PROBE flag. Submit mode refuses the third callback before
sending GPU work; fence mode substitutes an invalid local observation without
writing the read-only fence mapping. Require the application's Present to fail
and GetDeviceRemovedReason to report device loss, rather than successful pixels.
No hardware hang/reset is induced. Preserve baseline restoration and bounded
runner timeouts. A crash, timeout or undetected loss rejects the candidate.

Apply loss-icd.patch and loss-umd.patch after the Present patches.
loss-manifest.json binds UTF-8/LF inputs and outputs. M544 records027-029.
