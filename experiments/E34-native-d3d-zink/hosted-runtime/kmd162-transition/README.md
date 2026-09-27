# KMD162 diagnostic transition

Candidate final002, isolated68fc8f2, SYS58189FA6, version0.7.162.1/ABI0x000700A2.
Exact161/40F7916F rollback. final001/89A7AE28 remains preserved and unshipped.
Only bounded rejection/root diagnostics and version fields differ from exact161.
No admission relaxation or completion without work. Observe gates0/interop0 here;
BGP1 is a separate bounded trial after baseline validation.

Preflight requires exact161, health15, guard0, gates0, no workloads, STOP clear,
clock owner unchanged and temperature below85C. Stage exact candidate/rollback
hashes; PS5-parse scripts and check staging-hashes before Prepare/Start. Commit
before dispatch. Track worker/installer/collector PID+start and terminal receipts.
No restart solely on observation timeout. Preserve current OS boot. Confirm
exact162/hash, baseline UMD/ICD, healthy automatic confirmation before probe.
No deployment or G0 acceptance is implied by the existence of these scripts.
