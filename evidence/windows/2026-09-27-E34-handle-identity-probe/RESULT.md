# Allocation identity probe preparation

M623: compile-only validation of new callback diagnostics. All12 quick gates and
actual WDDM compilation pass. The probe is default-off and no runtime invocation
has occurred. Exact source hashes and unchanged build logs are retained.

Historical evidence already includes user-process BC2A opens with NULL ordinary
GetHandleData, not only CDD opens: [E25 fill-first log](../2026-09-22-E25-m8-address32/address32-results/fill-first-kmd.txt),
lines269-272 show allocation4096 bytes, private192 bytes and flags Create.
This supports broadening the investigation; it does not establish the cause.

The bounded probe compares ordinary lookup with WDDM2 acquire/release and live
allocation membership. It records callback offsets/pointers, not module ownership.
Successful acquisitions are released before returning. Neither acquired results
nor diagnostics change GPU Present admission. See the experiment procedure for
remaining runtime controls. No deployment, callback success or G0 acceptance.
