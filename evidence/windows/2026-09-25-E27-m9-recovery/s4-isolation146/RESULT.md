# M466 - Second retained S4 with summary polling paused

Unit A, 2026-09-25. Installed KMD0.7.146.1 SYS
`1B331C2C743A1DDD3072C262E2E200C82BF6E00F260FE1C7191DADE3D57CCCE7`,
unchanged from M463. M464 monitor pause was active through this trial.

OS Power-Troubleshooter records sleep2026-09-25T01:40:18.4463969Z,
wake01:42:37.9285530Z and EffectiveState5. Original process7720 retains
context, allocations, GPU virtual addresses and fences. Both full64MiB
readbacks match, fence64 to128, without refill or recreation. The validator
intentionally leaves actual_s4_verified false: its input proves retained
contents; the separate OS power record proves S4. Eight shaders and
stories15M/TinyLlama reference outputs pass (control.log).

The owner reports regular cursor stutter despite the summary pause.
Disabling synchronized LOG_SUMMARY polling alone did not eliminate the symptom.
343 complete CPU samples were recovered; a139-byte trailing NUL-only tail
is excluded by the analyzer and the original file is preserved privately.
After resume median CPU3%, DPC and IRQ median0%, p95 0%, maximum1%.
The interval includes model tests and WPR setup/rundown, so CPUmax100%
is not evidence of the cause of idle cursor stutter. Coarse samples cannot
exclude short stalls.

WPR GeneralProfile, GPU and DesktopComposition trace was saved successfully;
124780544 bytes are local for offline analysis. No precise interval of active
cursor movement was recorded. Raw ETL, power XML, driver logs and process
metadata remain outside the public repo at scratch/m9/s4-isolation146.
Raw trace paths/process identities are not published. No ETW conclusion yet.

Later SSH became unreachable. Recovery AC off03:56:49.291/on03:57:20.706
(local+02:00) produced verified boot03:57:55.500. Network failure alone does
not establish a GPU hang. Cleanup removed the bounded isolation tasks and
summary pause marker; WPR is inactive. KMD146 remains installed. Health at
04:00 flags7, native1000MHz/VID116,68.750C. Automatic confirmation pending
at that observation. M465 development paging-preemption fix is not deployed.

Result: retained data and compute pass again; interactive S4 resume is NOT
accepted and M9 remains incomplete. Analyze the trace before another S4 run.
