# Candidate 0.7.114.1 startup boundary trial

Hypothesis: the RLC busy transition on warm startup first appears during GART configuration or invalidation, before PSP loading.
Use the recovered Windows boot 2026-09-24T06:29:13, initially113 display-only. Verify closed gates and no full startup in this boot before installation. No routine AC baseline.
Install exact114 SYS82D1C6640D512A33FC4037549AD1258DB0A926A8C5A6A9C6A2711455D658414F with gates closed. Verify clock1000MHz/820mV and temperature below85C. Reset gate stays0.
First full startup, unchanged64KiB residency/content control, collect logs. Close gates and perform instrumented PnP stop. One warm full startup with preGART and substep snapshots. If successful repeat content control with a fresh output path; if unreachable preserve stream and recover once with identified plug, then collect persistent logs before closed-gate recovery.
If hypothesis holds, preGART is clear and a later observed boundary is busy. If already busy beforeGART, that excludes attribution to this observed initialization interval. A first changed interval does not prove a particular MMIO instruction caused it. Observation adds reads and latency.
Evidence records exact boot/version/hash, native content output, snapshots and power/SSH state. Power alone is not liveness evidence. General DMA/cache/alias/performance and repeated-start acceptance remain open.
