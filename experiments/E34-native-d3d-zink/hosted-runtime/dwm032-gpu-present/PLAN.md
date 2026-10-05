# DWM032 - shared startup log in a process without a console

Repeat the exact164/UMD5C74BF98/direct ICD3508416F bounded GPU Present trial after
correcting stderr reinitialization. DWM031 used _dup2 onto _fileno(stderr), which
is -2 without a console; the actual detached control rejects that redirect. Use
non-secure _wfreopen to initialize the stream with shared access. Positive controls
must prove both content sharing and success with an initially missing descriptor.

Up to30s startup, then180s measurement, stable identity and exact module hashes.
CPU image readiness, GPU Present/error checks,300s watchdog and durable rollback
remain. Registered baseline CF3948D6 is restored; exact163 rollback verified.
Hypothesis: an initialized shared stderr lets readiness observe GPU startup.
Multiple/newly replaced DWM identities still fail; do not mask a router failure by
loosening that gate. Record readiness UTC and inspect device/process ETW afterward.
No G0 pass without correct content, DWM GPU ownership and no full-frame CPU copies.
