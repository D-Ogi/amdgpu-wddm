# M405 - Second warm start and GPU content pass

Driver0.7.127.1, same Windows boot2026-09-24T11:44:14 as M404.
Second stop and warm start returned native SSH0. Stop retained disabled GART;
full startup completed. Post-start64MiB test passed three residency cycles and
four full GPU readbacks. validation.json records completion counters and limits.
Final independent info/confirm/log returned0; initialized full session retained.
No Windows restart or AC transition. Two total warm successes, including M404.

Plan deviation: the third warm trial was not submitted. The owner prioritized
visible full-WDDM display (M13.1), so further lifecycle repetition was deferred.
This result does not close M9 or establish accelerated desktop correctness.
Text logs decode UTF16 where necessary, preserve line endings and redact only
interface/PCI instance identity. Sources are the unchanged127 package and M404
probe, plus prepared warm2 script copied here.
