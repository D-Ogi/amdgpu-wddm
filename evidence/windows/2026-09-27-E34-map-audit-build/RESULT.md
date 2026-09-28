# M683: growable map audit candidate

PROVENANCE: Mesa, MIT. Source and artifact hashes are in build-receipt.json.
Parent9f8a1ff9 retains the opt-in uploader event audit from M680. This change is
host-only preparation, not a new lab result or GPU/no-copy acceptance.

M682 exhausted128 aggregate map buckets and lost identity for12 image-map
requests. The new audit table grows128,256,... rather than silently reaching that
fixed limit. Allocation failure leaves earlier records intact and still increments
the explicit overflow counter, so existing evidence checks continue to reject
incomplete coverage. Bucket IDs and output fields remain compatible. A per-context
mutex serializes aggregate counters, bucket updates and snapshots. Storage and
mutex are released during context teardown; audit-disabled contexts allocate none.

The actual storage helper compiles as C11 with /W4 /WX. Its host control inserts
4097 different keys (capacity8192), checks exact4098 requests/33579016 bytes,
initial allocation failure, failed growth with preservation, duplicate accumulation
without allocation even during injected OOM, and a duplicate after explicit storage
relocation. The complete UMD target rebuilds23 steps successfully. The control's
observed console result is recorded structurally in the receipt; it is not a lab
transcript. Parallel live-map stress and runtime regression remain unmeasured.

This removes the fixed-capacity failure mode; it does not add resource-lifetime
identity, count stores through retained pointers, establish map success/unmap
lifetimes, or provide a final interval-boundary snapshot. Those remain required
for the G0 copy audit. No lab configuration or baseline changed.
