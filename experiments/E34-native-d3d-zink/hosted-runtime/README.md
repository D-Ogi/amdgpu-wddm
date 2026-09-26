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

Local build: both DLLs link; all eight fast gates pass. Runtime result pending.
