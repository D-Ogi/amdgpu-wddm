# Opt-in uploader audit: build and positive control

PROVENANCE: Mesa, MIT. Source revision and artifact hash are in build-receipt.json.
The isolated source branch adds BC250_UPLOAD_AUDIT, disabled by default. It records
manager identity plus creation timestamp, resource, generation, buffer capacity,
suballocation offset/size and cumulative successful allocation/helper-copy bytes.
Each successful allocation emits a record; no periodic sampling is used here.
Release/create/destroy events describe lifecycle. Callers' later pointer writes
remain outside the helper-copy counter. No lab deployment or G0 pass is claimed.

The full Zink D3D UMD build passes. The host control includes the actual changed
u_upload_mgr.c with genuine Mesa headers; only pipe resource create/map/unmap/
destroy callbacks use heap memory. It links the baseline Mesa utility and C11
compatibility libraries. There is no window, driver access or GPU work.
The control verifies copied bytes, fills a4KiB buffer, forces rollover to a new
buffer, allocates an additional range without copying, fails an initial allocation,
and repeats a copy with auditing disabled. Event-log decoding independently checks
4 allocations/4144 bytes,3 copies/4112 bytes,2 buffer generations and distinct
creation stamps despite manager-address reuse. All3 created/mapped resources are
unmapped/destroyed. Earlier recipe/compile/link failures are retained privately;
the final build and actual execution logs are retained here.

The candidate retains previous rendering behavior with auditing off. Its runtime
behavior, log overhead and integration with explicit measurement/capture boundaries
remain unvalidated. Previous sourcea0ad8af5 and UMD5C74BF98 are unchanged and remain
the known runtime candidate. Do not substitute this host control for a desktop run.
