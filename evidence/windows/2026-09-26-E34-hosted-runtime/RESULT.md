# M539: hosted RADV allocation, submission and fence control

Run008 exits 0. Two native D3D CreateDevice entries independently create a hosted
RADV logical device, allocate a 16 KiB buffer, submit a GPU fill, wait for its
fence and read 4096 exact words with zero mismatches. Each hosted device and its
paging queue is destroyed before the D3D entry returns. The unchanged standalone
Zink control also passes three 4096-pixel checks and survivor-device rendering.

The bridge receives allocation, map/residency, lock/unlock, context, submission,
GPU signal, CPU wait and destruction callbacks from each matching runtime device.
No wrong-thread diagnostic occurs. Runtime context HANDLEs are stored without
truncation; 32-bit tokens are used only within the private bridge.

Run007 exposed a conversion error: MapGpuVirtualAddressCb returned E_PENDING,
which was treated as failure. The local WDK reference documents E_PENDING as a
successful asynchronous request requiring a paging-fence wait. Run008 maps that
result to STATUS_PENDING for map and residency, then performs the existing wait.
Reference: ref/ddi-display/d3dumddi.md, MapGpuVirtualAddressCb and MakeResidentCb,
WDK/SDK 10.0.26100. No KMD or firmware change was needed.

Exact run008 artifacts:
- Hosted ICD 9ED551B8F90F909D7D1F76A3828A464CFEF2EAA39B2E869F5EC3560A991E29D9.
- UMD 490082C3C172B53C26CBDFCB4C2073CF5844052CA193D0C81AE7871BDF9839CE.
- Control D624FAE36995D3F77AD21379261FCE9512838AEEF8BC695A4571B712B9796704.

Source base is bc250-win 36b5fe2 plus the E_PENDING correction captured in the
updated hosted-runtime patches and manifest. Both DLL builds pass; all eight
fast quality gates pass. Logs and script preserve the runtime checks and hashes.
Only the test process selected the candidate UMD. CPU UMD8279AC7F and registered
ICD9C40083C were restored; DWM PID1052 stayed unchanged. Pre-test temperature was
66.4 C, STOP was clear. No restart or clock change was performed.

This establishes bounded hosted GPU execution. The hosted VkDevice still lives
only inside CreateDevice; subsequent Zink rendering remains standalone. Native
Present synchronization, persistent callback lifetime, GPU DWM attribution and
exclusion of full-frame CPU copies remain open. G0 is not passed.

PROVENANCE: Mesa MIT; WDK/SDK10.0.26100 declarations. No private dumps collected.
