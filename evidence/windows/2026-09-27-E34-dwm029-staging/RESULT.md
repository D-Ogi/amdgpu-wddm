# M662 - DWM029 staged with durable backups and gate-first rollback

Runner targets exact1649B9B99D3 (M661) with unchanged hosted UMD5C74BF98 and direct ICD3508416F. RouterCC6AAFAF/control63871A69 compile with /W4 /WX. Final18-file manifest is included; stage002 differs from001 only by fixing the cleanup message trial number. No trial started and KMD164 is not deployed yet.

The M659 rollback failure motivates these changes:

- Copy source hash, CreateNew destination, WriteThrough, Flush(true), destination hash before candidate activation. Existing backups are never overwritten.
- Persistent Present/CDD gates are closed via checked RegFlushKey before backup validation or driver escape queries. DLL restoration and gate rollback are both attempted even when one reports failure. A missing pending marker returns before adapter queries/restart.
- Already-restored DLLs do not depend on a backup remaining valid. A damaged baseline copy can fall back only to a hash-verified original. Candidate/unknown active files are preserved; unknown content is not overwritten.
- Completed collector samples are flushed and contain forced hardware summary counters, preserving startup diagnostics without waiting until the trial ends.

PS5 host controls exercise real helper functions: matching copies, wrong hash rejection, no overwrite of existing backups/markers, valid-original fallback from a damaged backup, preserved candidate, already-restored path without backups, and refusal to replace an unknown active file. All pass. These tests verify code behavior and successful flush calls, not a destructive power-cut experiment.

Target11:45:21Z: all staged hashes and PS5 parses pass, exact162 rollback exists, no-marker restore leaves gates0 and no trial marker. This also executes checked native registry flush on the lab. Post-stage11:45:35Z: exact163 retained, CPU DWM2016, same boot, health15/guard0,1000MHz/VID116,67C, no test process. Stage and local test scripts do not launch a window or mutate the host registry.

The next action is the separate164 transition with gates0, native regression, then a claimed-slot180-second DWM029 trial. GPU admission completion and BGP1 image content remain separate evidence requirements. G0 remains open.
