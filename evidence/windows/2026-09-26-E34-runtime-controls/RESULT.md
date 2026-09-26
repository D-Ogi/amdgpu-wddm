# M535: runtime resource controls and per-device screens

Three requested controls completed on unit A without OS, PnP or DWM restart:

- Callback input hRTResource is nonzero and unchanged by AllocateCb. Shared
  texture allocation succeeds but hKMResource remains zero. Cause is unresolved.
- Actual compiled UMD layout equals the separate WDK 10.0.26100 utility:
  size 40; offsets private 0, privateSize 8, hResource 16, hKMResource 24,
  NumAllocations 28, allocationInfo2 32. Ninja dependency output confirms both
  DDI headers come from the WDK NuGet package, not substitute Mesa declarations.
- Shipping CPU UMD returns a nonzero GetSharedHandle; a second native D3D device
  opens the texture and receives its expected 64x64 RGBA8 descriptor, exit 0.
  The GPU candidate also returns a nonzero GetSharedHandle even though its
  callback reports hKMResource=0. OpenSharedResource then fails with 8007000e
  in the unsupported USER_MEMORY resource import path, application exit 9.
  This separates app-visible sharing from callback hKMResource observability.
  No sharing pixel-coherence claim is made by this handle/descriptor control.

A subsequent native GPU control uses two separately owned Zink screens. Red and
blue clears have 0/4096 mismatches; after destruction of the first D3D device,
its survivor renders green with 0/4096 mismatches. Exit 0 including destruction.
This verifies per-device screen lifetime only: RADV still shares its standalone
winsys; hosted dispatch and runtime-allocation GPU rendering are not implemented.

UMD SHA256: 704DCFFB7E9ED7353D35D2527E947F846533248D6803FCD73BA071093529ADEE.
Sharing control: A121D43EF6227E5DCC8AAD477F270E6F6D0CE8ECFD275795961594F2475691AF.
Two-device control: D624FAE36995D3F77AD21379261FCE9512838AEEF8BC695A4571B712B9796704.
ICD M532 7A9970CA during each candidate run. CPU UMD8279AC7F and ICD9C40083C
restored and hash-checked; DWM PID1856 unchanged. All tests are offscreen.
G0 is NOT passed: no GPU DWM composition, native Present or no-copy proof yet.

Patch is incremental after M534. Raw diagnostic output is preserved, including
ShareObjects(0) rejection (not a valid independent test of export support).
