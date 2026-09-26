# Borrowed allocation residency regression

Unit A, 2026-09-26. Source base bc250-win d4712e5 with borrowed-residency.patch
applied after M541 runtime-import patches. The adjacent manifest identifies
exact ICD, UMD and unchanged shared-color control binaries. Mesa MIT.

RADV destruction previously reached Evict before checking bo->borrowed. The
new guard excludes borrowed allocations from that eviction. The UMD retains
residency, GPU-VA and allocation ownership. RADV still closes any mapping it
created before releasing its BO wrapper. This is a source-level ownership fix;
the earlier successful color test did not establish correct residency ownership.

run019 exits 0: red, blue and survivor green each match all 4096 pixels.
Both devices tear down, and DWM remains PID 1052. The runner checks candidate
hashes before use, hashes the actual control executable, and restores CPU UMD
8279AC7F and registered ICD9C40083C. It attempts system UMD, system ICD and
app-local UMD restoration independently, collecting errors instead of skipping
later restoration after an exception. Error-injection recovery is not tested.

ICD builds and all eight existing scoped fast gates pass. This run is a content
and teardown regression, not a direct per-allocation Evict trace or a lifetime
proof. No native Present, cross-device pixel, GPU DWM or G0 claim follows.
