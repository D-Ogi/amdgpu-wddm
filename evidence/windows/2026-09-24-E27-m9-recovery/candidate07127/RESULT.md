# M404 - First successful warm127 start followed by GPU paging and inference

Unit A,2026-09-24. SYS476858800F28A5C103E43FECCC8595C6042BA2479EE18FCFCC3D9FD313953E90,
version0.7.127.1, installed into recovered boot11:44:14. STOP absent; clock,
temperature, installedhash/version preflights pass. No OS or AC restart in this
trial. Full WDDM records pp_gfxoff0, the AMD no-GFXOFF RLC startup policy.

First full start completes and64KiB3cycle GPU residency/readback passes.
Stop confirms SDMA pre/post-reset quiescence0, GFXHUB retirement0/fault0,
GART hardware disable and retained-disabled final state. Display-only stage61
returns with CLI0 in unchanged boot. One warm full start then returns normally,
all request/dummy-read/ACK/MMHUB/publication checkpoints completed, nativeSSH0.
Full adapter remains admitted and guard confirmation returns count0.

Warm firmware command2 still first observes RLCbusy, all11PSPcommands succeed.
After bootstrap translation commit, RLC_CNTL1/STATUS2 8 and GRBM_STATUS3028 are
observed with readstatus0. Thus the earlier pre-RLC busy sample alone does not
predict failure; the no-GFXOFF policy admits working RLC/GPU operation in this
trial. This is a measured policy-associated recovery, not an independently
proved internal firmware root cause or repeated-start guarantee.

## Positive workloads after warm start

-64KiB,3eviction/restoration cycles,4full-range GPU readbacks:PASS.
-1GiB,3cycles,4full-range GPU readbacks reaching fences1024/2048/3072/4096:PASS.
 Each1GiBcycle reports NOTRESIDENT3 after pressure, GPU residency1 after restore.
-8M8shader tests,3runs requested each:all printed GPU hashes equal CPU hashes.
-stories15M96tokens,7/7GPUlayers, andTinyLlama64tokens,23/23GPUlayers: native0.
 Outputs match immutable E14LinuxGPUreferences after CR normalization.
 Input/model hashes and exact cache-intent-v2ICD hash/loader witness retained.

Final independent info/confirm/log observation returns0. CumulativeGFX6454/6454,
paging197311/197311,0timeouts/0refused,noTDR.74reservedcaptures/0heap,
per-context peak1plan/5505328reservedbytes. These workloads do not prove the
universal capture-resource bound or exact cache/PFN alias ownership.

All native tool sessions terminal; inference scheduledtask removed. Retained
full127session, boot11:44:14,stage50,count0,executiongatesenabled. On-diskFull0
is consumed one-shot, not display-only. SSH remains responsive at finalcheck.
USBloaderOFF,plugON; no recoverycycle was needed or performed.

## Evidence limits and collection

Only one stop/warm sequence is measured here. Further repetition, partial
failure/OS lifecycle cases, idle stability, cache/PFN contracts, universal
pagingresource/status/dependency guarantees and Windows-over-Linux performance
remain open. This is content validation, not a new benchmark.

See COLLECTION-NOTE.md: initial text copying doubled CR characters, causing
a local comparison artifact. Authoritative inference-native/ and
residency-native/ copies use explicit newline preservation; validation.json
checks these against full byte-oracle markers, completions and E14references.
Original copies remain unchanged. UTF16 summaries are decoded toUTF8 and
PCI/interface identities redacted; no workload repeated for the copy issue.
SHA manifest includes all files recursively. The run logs retain partial
observations as well as completed outputs; those partial observations are not
independent additional successful tests.

Next preserve this successful initialized session. Extend controlled warm
repetition with post-start content checks, then resume remaining M9resource,
cache/lifetime and matched performance work. Do not mark full M9 complete.
