# DWM031: ambiguous startup identity, empty router log (M669)

Unit A, 2026-09-27, runner7d383f7. Fresh exact164/new CF3948D6 baseline verified;
20-file target staging and PS5 checks pass12:37:06Z. One launch12:37:30Z.
Readiness reports more than one new DWM identity and aborts before measurement.
The router created an empty dwm-7068.log; no route log or hosted-ready receipt exists.
Do not interpret this as a passing GPU desktop interval. Actual transition between
DWM7068 and10648 needs ETW correlation; the absence of a ready sample limits the
precision of this record.

Automatic rollback finishes12:38:03Z, CPU11616; done12:38:06Z records zero measured
seconds. Collector9904/start12:37:48.1707598Z ends12:40:49.2739080Z,159 samples,
no reader timeout. After identity-matched absence, archive pulled and all031 tasks
removed. Fresh12:42:17Z verifies exact164/baselines8279AC7F+CF3948D6,health15/guard0,
1000MHz/66.750C,same boot,no test workload. Cleanup verifies registry/latched gates0.

A subsequent review identified that the new router used _dup2 onto _fileno(stderr),
which can be -2 in a process without a console. The next host control tests that
specific process environment; console sharing controls in M668 do not prove it.
No production UMD/ICD/KMD change or G0 acceptance. Raw archive/ETW retained privately
with hashes; no raw evidence was edited.
