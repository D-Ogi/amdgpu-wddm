# M588: flush-only control and DWM startup failure

Diagnostic UMD91345C63/ICD3508416F on unit A, mode2: flush after each
draw, skip the added CPU fence_finish. Control109 passes56 state images,
229376 pixels and3584 draws plus the existing8 graphics cases. Three log
witnesses confirm mode2. CPU DWM2372 was unchanged and baselines restored.

DWM018 then fails its first5s startup check: GPU UMD witness missing for
process8196. The measured interval is zero. No router log or claim file was
collected. The runner checked the module list before saving it, leaving an
evidence gap; the next run must retain startup modules before enforcing this
gate. This is not a measured comparison of flush-only desktop correctness.

Rollback and cleanup verify baseline ICD93B1D1FD and CPU UMD8279AC7F;
CPU DWM8892, all018 tasks removed. No promotion, BD-043/G0 remain open.
Raw images/ETW/logs stay private; their hashes are retained. No crash cause
or rendering fallback cause is established by this startup failure.

PROVENANCE: Mesa (https://gitlab.freedesktop.org/mesa/mesa), MIT.
