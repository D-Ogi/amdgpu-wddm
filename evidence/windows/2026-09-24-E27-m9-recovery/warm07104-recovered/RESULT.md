# M332 - Warm07104 persisted evidence recovered

Read-only recovery after restoration of AC. Owner reports Windows desktop; pinned SSH identity rediscovered. Current boot is 2026-09-24T03:06:12 local. Installed SYS hash matches candidate 0.7.104.1: 9EE1AA001125B414DC7CBB0382ABF6838427223D8130F1A89505D402D32B69CF. Current adapter is healthy display-only, EnableFullWddm=0, LastStage=61, UnconfirmedStarts=2.

The failed M330 warm startup's last persisted snapshot is ring-20260924-001459-614.log, SHA256 32998FAC00A9E4CFD9B16137B3B34AEAFA83CEA11479F561A30208B8C1B6C18D. It ends at CP1 scheduler-read, line 65, elapsed 0.393 s. No scheduler-write/done follows. Prior startup stages, PSP commands and RLC stage report success. GFX stage 5 reports seven writes, compared with eight during M328 first startup; this is an observation, not a causal diagnosis.

Stop snapshot ring-20260924-001455-136.log reaches stage79 after CP/MEC/SDMA halt values, PSP destroy-TMR/ring-stop success and 282 GART restoration writes. Node0 completed34/34; paging25559/25559; no timeout/refusal/TDR. This still does not establish RLC retirement (M331).

Recovery stream includes per-file before/after hashes and reports stable files. Original successful-start checkpoints later in recovery.log belong to before.log and MUST NOT be attributed to the failed warm startup. Dump metadata contains no new dump from this trial.

The first installed-image hash probe used an incorrect assumed path and produced an empty result, preserved as installed-sys-sha256.txt. identity.log supersedes that failed observation using the service ImagePath, with successful exact hash readback. No driver replacement or GPU initialization was performed during recovery.

Next: distinguish the synchronous GuardLogKeep completion boundary from the scheduler MMIO read and compare warm RLC/KIQ entry conditions against reference code. Do not infer the exact hanging instruction from the missing next persisted snapshot. Full warm-start acceptance remains open.
