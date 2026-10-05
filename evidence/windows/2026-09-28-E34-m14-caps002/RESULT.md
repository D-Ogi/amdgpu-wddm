# M14 exact-pair capability control002

Unit A, 2026-09-28. Engine source8035213422260da9602ff28a991e194cdda79040, test sourcebf14ecca0700d563482f633e24ab10e306583234. Frozen set bf14ecca-253A41AA carries the engine unchanged. PROVENANCE: DXVK upstream, zlib licence; Mesa RADV, MIT licence.

Hypothesis: the directly loaded packaged RADV can run the engine control and provide the adapter's maximum-level capabilities for the shell's configuration. Positive control: the same direct-loading test passed on the development GPU; packaged RADV created an instance there and correctly reported no matching BC-250. Those control receipts remain in the frozen artifact workspace. This lab run uses no validation layer.

The wrapper verified the engine, test and ICD hashes before launch, passed --icd with the absolute packaged path, polled STOP every500ms and temperature every5s, and bounded execution to120s plus5s termination. No environment-based driver selection, registration or driver change was used. The test's in-process module witness identifies exactly one ICD, with the expected path and SHA256. See stdout.txt and caps.json.

Result: exit0, PASSED:0 failure(s),5.6968495s. Maximum feature level0xb100, doubles1, compute/raw/structured1, logicOp1, tileBased0, minimum precision2/2. These are answers at the adapter's maximum level. The engine test also checks image contents and reports all of its controls in stdout.txt.

The configuration writer at repository1a7b7081 validated caps.json against the exact engine and ICD files and produced the enclosed108-byte bc250d3d11.config, SHA256 EAB1F55AAC51C56A42DA2231C23DCDEFCF6C00FCB06D4DF6B1EB7A4059E7CBF2. This file was not installed. The engine, test and ICD hashes are in caps.json. The shell DLL was not loaded in this control.

Pre/postflight both passed CPU170/SYS67F0241506200D1053E6136ACDEA54F447BA945E65AA11C7EB250B8ECE195503, health15, unchanged generation81058018120/epoch5, same boot and DWM, temperature66.9/67.0C. No test process remained; no TDR or CollectDbgInfo was reported. Registered CPU UMD/ICD and desktop gates remained unchanged. Lab slot closed.

Limits: non-hosted Vulkan instance/device; no Microsoft D3D runtime, system DDI, hosted callbacks or native Present. This supplies an exact-pair configuration input, not M14 completion, deployed FL11_1 validation or a performance comparison.

stdout.txt, result.json and caps.json are unedited. The local pre/postflight records include the diagnostic ring; that ring and allocation-handle stderr are excluded from this evidence selection. The raw source records and runner remain in scratch/m14/lab-caps002.
