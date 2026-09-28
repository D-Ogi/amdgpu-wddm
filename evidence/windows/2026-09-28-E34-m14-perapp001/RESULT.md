# M739 - Same-GPU per-app DXVK matches the native M14 images exactly

Perapp001 source cf3a6444, manifest
B2FEE5D0B20E531515212BDAEA4DB79C799E6AA931623189045C4917F8C16E8A.
Fresh per-app DXVK bf14ecca uses D3D11 SHA256
41CE2364783DB0DD3291F24516C181329471668FAB8F974AE255AA572A61CDBF
and DXGI F593D6C2C7F3ABE312094DA1A7F93BD51C6EE6AA7A44EDA1C84BBCBBA2003F81.
Its source differs from native engine80352134 only in the engine test and its
Meson file, not the rendering implementation. Client84C328CD and ICDC0CE are
frozen from native runtime011. Exact ICD and application-local API modules
were witnessed in the GPU process; native CPU UMD was absent from that route.

All three GPU images match native runtime011 byte for byte: draws
ba148e83316c6a24, fill73736636b17371e9, shaders211d40e09e1c3c64.
The CPU control also repeats all three corresponding runtime011 images
byte for byte. Independent host decoding reproduces all six JSON checksums;
image-verification.json records each PAM SHA256. Both clients exit zero,
frame-scene API/query/disjoint failures are zero. Strict gates pass without
tolerance. Workload remains64x64,8 draws,2 layers,4 shader variants,3 frames,
zero warmup; the shader target is256x256.

This establishes native/per-app equivalence for these measured scenes on this
GPU and ICD. It does not prove all D3D11 operations conform, nor classify every
cross-implementation arithmetic difference. Runtime010/011 cross-CPU gate
failures remain recorded, not retroactively turned into passes.

Independent supervisor passed50.0847336s with baseline/postflight/tree closure
verified. Original registered ICDCF39 restored, CPU171 unchanged. Cleanup
succeeded; subsequent Inspect13:25:37Z reports task Missing. No KMD/DWM or OS
restart. Selected JSON omits environment, full module paths and timings;
PAMs are synthetic benchmark images. Raw material scratch/m14/perapp001-ops.
The debugger and short offscreen workload do not establish window Present,
no-copy behavior or the performance bound. Those remain next requirements.
