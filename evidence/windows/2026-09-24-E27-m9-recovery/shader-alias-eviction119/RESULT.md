# M384 - Shader-produced aliased data survives paging

Measured unit A, same initialized119 session, boot2026-09-24T08:30:00,
1000MHz/820mV; no restart, PnP transition or power action. ProbeSHA256
F3721C0600BAB71EDB83A53AB0468086DC5B24D6880BBA476232E2BFBDE7FB5B,
diagnostic ICD EA70D44045BF77867C9BE50EFB311E7CE7C83D01F4B527FB190E5BC8BD3F8306.
Exact source and E14 reference, build log, scripts, native logs and independent
validator are retained here. Native loader/submit witnesses checked.

Two distinct VkBuffer views share one4MiB type3 coherent allocation at offset0.
Baseline16rounds pass. Eviction variant also passes16rounds with identical CPU
and GPU hashes: at rounds1,5,9 producer shader completes before unmap triggers
actual residency departure2/3 and restoration1. Consumer runs through the other
buffer view after a global shader-write/read dependency. No intermediate host
write occurs between producer and consumer; passing proves produced content
survived these witnessed cycles. Three departures and three restores confirmed.

Stale-input control passesround0, then after its first eviction round1 reads
previous input's double hash6747a9c9b5e067b5, differing from new CPU reference
d7bd083bb8a604c8. Expected nativeexit1/completed1/mismatch1. Both positive native
exits0. The test distinguishes data preservation from a vacuous overwrite after
restoration; it is not a performance measurement.

After: GFX13340/13340,paging209594/209594,0timeouts/refusals,noTDR,
134reserved/0heap captures. See validation.json for full before/after counters.
SSH wrapper and independent final CLI info/confirm exit0; hidden Limited task
removed and initialized119 session retained. Host run/final logs redact irrelevant
hardware/interface identity lines and PCI identifiers; native outputs unchanged.

This closes the specific M383 missing eviction coverage for same-memory Vulkan
views. It does not establish distinct GPU VAs, physical relocation, all CPU cache
aliases, arbitrary cross-page paging-copy dependencies, unbounded pressure, or
warm reentry. Local120 remains undeployed; full M9 acceptance remains open.
