# WDDM detach before DPC drain, local0765

2026-09-23. Source base HEAD bed764d with captured uncommitted changes. No lab access.
Bc250InterruptRoutine calls IH/DCN ISR routines, neither reads Device->Wddm.
Bc250DpcRoutine then calls WDDM fence/vsync/report handlers. StopDevice calls WddmStop
before IhStop, so IH may still queue a fresh DPC around WddmStop's flush boundary.

Old order cancelled timers/DPCs, flushed, then detached Device->Wddm. A new DPC after
the flush boundary could load the still-published object and retain it past free.
New order marks Stopping, cancels private timers/DPCs, atomically detaches the pointer,
then flushes before summary/free. Previously admitted DPC readers are joined; later
entries see NULL. Synchronized notify callbacks use device callback state, not WDDM.
Non-DPC DDIs still rely on dxgkrnl StopDevice serialization, as before.

Host harness extracts the actual cancellation/detach/flush sequence and injects a
new reader at the end of a modeled flush boundary. Old code fails the stale-reader
assertion, new code passes. Earlier-reader completion, cancellation counts and
armed/disarmed vsync controls pass. This deterministic ordering model does not
exercise actual Windows interrupt scheduling, runtime teardown, or the full DDI
synchronization contract. Kernel compilation/signing passes.

Candidate scratch/build/bc250kmd-0765/package-umd, version0.7.65.1, SYS SHA256
1F8CE439605E048E8D9EE10A9EA64DC4A5989E12AB6A47CBA88BE3F8C07D575D.
Not deployed. This is CPU object lifetime only; it does not establish GPU halt/reset,
resolve ring-refusal software completion or implement system-memory paging.
No redactions. Full source and raw outputs included; source-before input archived.
