# M378 -119 shader visibility after eviction passes

Unit A, Windows2026-09-24, retained119 full-WDDM session boot08:30:00 from M377. SYS4374CB20857D0CA5D09D8EFCF61F5D35718633D568571EF41FE717900E15236E, clock1000MHz/820mV, observed temperature about70C. No driver reinitialization, install, OS restart or power operation.

Unchanged shader-coherency-probe SHA038010EB3E6220A12F66713BEC9B9DCC527646DA1AAE5E7D3880AADA2F467D9A and instrumented eviction ICD SHA EA70D44045BF77867C9BE50EFB311E7CE7C83D01F4B527FB190E5BC8BD3F8306. Actual loader path and submit progress verified in each native trace. Each run has3 buffers of4MiB, Vulkan memory type3/heap1/flags0x7. Limited interactive task runs hidden on lab and is removed after completion.

Positive run16 distinct GPU hashes all equal CPU. Eviction run16 hashes equal both CPU and positive run, with3 successful residency departures(value2/3) and3restores(value1). Stale-input control returns native1 as expected: initialround matches, round1 GPU keeps previous input hash and differs from current CPU expected hash, completed1/mismatches1. Its single forced departure/restore is witnessed. Thus32 positive rounds plus a discriminating negative control pass. See validation.json and raw native outputs.

Counters before/after: GFX512/512 ->546/546, paging37165/37165 ->40813/40813, no timeouts/refusals/TDR at either observation. Reserved captures12 ->32, heap0 throughout. Background Windows work contributes to cumulative paging counts; do not attribute all deltas to forced evictions. These results establish the tested shader visibility path, not exact CPU cache mapping attributes, PFN ownership, physical relocation or arbitrary aliases.

Workload and final observation streams exit0; independent info/confirm succeeds. CURRENT119 remains initialized full WDDM, boot08:30:00, stage50/count0, execution gates enabled, on-disk Full gate0 one-shot. No active probe/task/observer remains. Retain session for current-build inference correctness and performance validation. M376 post1GiB incident and warm-reentry failure remain unresolved. Host PCI/interface identity redacted; native outputs unchanged. Goal incomplete.
