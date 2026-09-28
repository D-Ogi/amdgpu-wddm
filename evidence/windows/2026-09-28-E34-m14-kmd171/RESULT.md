# KMD171 CPU deployment and adapter identity control

Unit A, 2026-09-28. Candidate source75b8ab6fa60d39d46211b06545ace8a4736eaf12 on m14/kmd171-resource-luid. Exact170/a3c2d2b base plus KMD portions of fabb4567 and3e2d6064: E26R v3 shared-resource metadata and optional1496-byte adapter caps query with OS LUID trailer. Existing170 VSync code/diagnostics retained. Full compile, prototype/ABI, stack-budget, catalog and test-signing gates passed; source manifest clean and deployment-source-eligible. Firmware/query host control2426 checks0fail.

Signed SYS65172CA1058A11335F1132644DA844748758E9BE017D5192B5CE1E9B424FEEDF, version0.7.171.1, ABI000700AB. Runner6613ae83; stage manifestCF57DB7375B4FE787CC98EF83D6D06F078185A34D041045BA3782A914B8C0E91. Candidate/rollback packages and all62 files are hash-bound. Existing exact170 package was retained as rollback. Sixteen local supervisor/gate tests passed before staging.

Hypothesis: this additive KMD contract preserves the CPU desktop baseline and supplies a LUID that agrees with user-mode enumeration. The measured170 deployment workflow is the transition control; contract host tests preserve old-sized queries and validate trailer layout. The runtime probe compares independent OS enumeration and the KMD's retained StartInfo trailer on the same live adapter.

The independent scheduled supervisor had a180-second hard task limit, process-tree closure, checked CPU readiness, rollback170 on failed admission and restoration of SetupAPI logging. Result: closed after99.0867556s, candidate verified and retained, logging restored. No reboot. The task was removed, and an independent inspection found it missing. No GPU workload or new UMD registration was part of this transition.

Independent postflight: exact version/SYS, CPU UMD8279/registered ICDCF39 unchanged, health15 generation305821861289/epoch5,66.9C, same OS boot, no competing process/task. The first postflight script mistakenly retained the old ABI170 literal and rejected the loaded171; a separate corrected read-only postflight passed. No transition retry followed this instrumentation error.

Read-only LUID probe SHA256A6ED1642E33EB116EE83D640D6C7A979F23246759C5C98EAE96FCC74FD35BEFA, built from scratch/m14/luid-probe/luid_probe.c with /W4 /WX /MT, completed under a15-second bounded child. Exactly one PCI1002:13fe adapter: EnumAdapters2 LUID00000000:032fd8c0 equals trailer LUID. Magic49413242/version1/size24/reserved0. Basic Render carried no trailer. Exit0, PASS. The same probe's development-PC controls reject a missing BC-250 and absent/refused trailers; the kernel host test checks known trailer fields. The post-transition RADV LUID has not yet been compared.

Limits: CPU baseline and KMT identity only. Hosted Vulkan/device creation, resource import and the Microsoft D3D runtime remain unmeasured on171. This is not M14 completion or GPU deployment acceptance.

watch-result.json and luid-result.txt are unedited. postflight-selected.json selects only version/hash/health/timing and task/process status, excluding the raw driver ring and device instance identifiers. Full local records and a1.74MB remote archive remain in scratch/m14/kmd171-deploy001-ops. Lab left on confirmed CPU171 with rollback170 preserved.
