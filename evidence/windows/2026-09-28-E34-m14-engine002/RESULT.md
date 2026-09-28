# M14 standalone engine control002

Unit A, 2026-09-28. DXVK engine source b418f55b3b1604f178202c4d724d67cd51d0f628. Frozen pair from scratch/m14/engine-artifacts/b418f55b-3CB26ABF, built with the ddi-engine recipe. PROVENANCE: DXVK upstream, zlib licence.

- Engine SHA256: 3CB26ABFD009A68B78BF3CDE12C8E7FE0611CE98C04DDBF0160092B8FE526AC3
- Test SHA256: 06D384025982BF1D39170D311A22DC043A1A3EEC792ACFA56BEA17D098075F74
- Registered standalone RADV ICD SHA256: CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157
- KMD170 SYS SHA256: 67F0241506200D1053E6136ACDEA54F447BA945E65AA11C7EB250B8ECE195503
- Shell repository revision at test: 4739298 (the shell itself is not loaded by this test).

Hypothesis: the inline DXVK engine can create its required Vulkan device on standalone RADV, consume DDI-form shaders and input layout, and render correctly into a caller-owned Vulkan image without queue submissions from another thread.

Positive control: this exact pair passed on the host RTX4090, both without and with Khronos synchronization validation, with zero validation messages in the latter. Frozen receipts remain local next to the artifacts. Unit A did not enable that validation layer.

Procedure: headless bc250dxvk_engine_test.exe with full engine DLL path and adapter selector AMD, DXVK_LOG_LEVEL=info, log directory under lab scratch. The wrapper checked both hashes, polled STOP and capped execution at120s; it retained the process handle through exit collection. No driver, registry or DWM changes. Independent preflight and postflight checked the existing CPU170 baseline, health15, unchanged driver generation and no TDR/CollectDbgInfo. Temperature67.0 C before and after. Process absent after completion.

Result: exit0 after6.8844256s, zero failures. Adapter AMD BC-250 (RADV GFX1013), API1.4.363; requested FL11_1,36 device extensions, queue family0. DDI-form VS/PS and register input layout render correctly to a caller-owned TYPELESS Vulkan image viewed as UNORM. Four sampled colors match, covered pixels2016. Last image remains correct after500 clear/draw/submits. QueueLock calls1006, foreign-thread calls0. No observed thread starts inside the engine DLL; final engine Release0. See unedited stdout.txt and result.json.

Limits: engine plus standalone RADV only. The system DDI shell, runtime allocation/submission callbacks, hosted ICD, Present/Blt/Rotate, SO and other shader stages are not exercised. The tiny64x64 timing is not a performance comparison or proof of the5% target. This does not complete M14.

Raw runner, stderr and DXVK log remain local at scratch/m14/lab-engine002; runtime allocation identifiers are omitted from this public evidence selection. No raw system diagnostic ring is copied here.
