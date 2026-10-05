# M757: standalone D3D12 engine on unit A

Frozen vkd3d engine ABI1.0 source 5712e8c5455fdfc5286295a6018c5bd5bcecca34 and RADV C388 pass engine001 on CPU171. Exact binary hashes are in receipt.json. Two independent devices create and release. GPU copy/readback and DXIL cs_6_0 dispatch each match all 16,384 words; the dispatch uses descriptor table slot 3. The measured descriptor increment is 64 bytes, not a portable constant.

Reported caps: maximum FL11_1, tiled resources tier0, resource binding tier3, ROVs, conservative rasterization tier3, shader model6.8 and raytracing tier1.1. These are capability queries, not exhaustive conformance or an RT rendering test. FL12_1 is not attained. Native D3D12 DDI, hosted queue ownership and runtime fence ordering remain unimplemented/unverified by this standalone test.

Engine5.642s, entire supervisor22.829s,66.9C; root exit0, Job empty, no timeout or STOP. Exact CPU171 pre/postflight identity agrees, including boot, DWM and generation. No deployment or restart. Positive content controls compare GPU results against CPU-generated expected words; ABI/unknown-adapter negatives also pass.

Raw output stays in scratch/m15/lab-engine001. checks.txt selects complete assertion/capability/result lines; adapter LUID, pointer/VA/handle lines and verbose driver stderr are omitted. receipt.json exports the result, artifact hashes and closure status without process identifiers. No claim of native system-D3D12 support, composition, performance or game acceptance.
