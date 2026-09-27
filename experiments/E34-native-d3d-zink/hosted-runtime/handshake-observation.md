# Bounded handshake observation worker

This worker is prepared for a reviewed successor of the initial redirblt-probe
26005fb/F4DA9E64. Do not run that initial artifact: the review found unresolved
worker/teardown lifetime, update-call contract and shared-handle ownership issues.
A future runner must pin the corrected artifact hash after review.

Stage run-handshake-observation.ps1, handshake-observation.ps1 and the reviewed
EXE in a new lab directory. Launch via an interactive scheduled task with a
120-second limit and the existing recorded-worker wrapper. The wrapper script
passes Directory, ProbeSha256, DwmUmdSha256, ExpectedDwmPid and the exact UTC
process-start string. Outer CPU/GPU runners pin KMD/ICD/UMD hashes, trace providers,
configuration, rollback and task cleanup as in DWM037. This worker changes no
registry, drivers or DWM process.

Run separate fresh no-paint and GDI-paint cases using --handshake-only --no-open,
with an odd641x479 client. Device identity and confirmed health are checked before
and after. The held child process has a45-second external bound, STOP polling,
and final termination if needed. CreateNew receipts preserve existing attempts.
The source allocation, context, resource open and Present must be absent in this
mode; the transcript classifier rejects source/open/Present records. Exit0 means
the observation completed, not that a GPU surface or presentation succeeded.
0x263005,0x263008,S_OK,adapter-not-found and no-surface are distinct observations.
Ordinal absence is recorded separately. ETW CDD allocations provide corroboration;
no tokens or actual engine copy are expected from this first test.

First run the matched CPU control, then the two cases inside a confirmed hosted
GPU DWM interval, retaining exact per-process identities. No GPU completion or
no-copy claim follows from the handshake answer alone. Present variants require
separate readiness/ownership review and known-colour plus ETW validation.

Host validation currently covers PowerShell syntax and pure transcript positive/
negative tests only. The worker has not been executed on the lab. No artifact hash
is defaulted, so this document cannot silently authorize a stale probe build.
