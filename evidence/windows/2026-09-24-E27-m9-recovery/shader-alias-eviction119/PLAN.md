# M384 - Preserve shader-produced aliased data through paging

Hypothesis: both VkBuffer views retain the same produced data after eviction and
restoration of their common VkDeviceMemory. Extend M383 probe. At rounds1,5,9,
submit producer alone and wait for its fence, evict/restore intermediate via the
existing diagnostic ICD unmap hook, issue a global shader-write/read dependency,
then submit consumer alone. Do not rewrite intermediate after restoration.
Validate all output bytes with E14 CPU double-hash oracle. This sequencing is
required: eviction before a producer overwrite would not prove data preservation.

Controls:16rounds alias without eviction;16rounds alias with3cycles; stale-input
negative control passesround0 and failsround1 after one eviction. Require native
0/0/1, actual diagnostic ICD hash/path,3/1departure+restoration witnesses and
matching positive per-round hashes. No distinct-VA or CPU cache-type claim.

Retain119 full session, exact SYS/version,1000MHz/820mV,temp<85C, STOP absent,
hidden Limited task, before/after counters, final independent CLI. No restart.
Save source/binary hash/build and all native logs. On failure inspect existing
workload, never rerun based solely on observation timeout.
