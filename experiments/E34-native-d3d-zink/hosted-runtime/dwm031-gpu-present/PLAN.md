# DWM031 - shared startup log and bounded readiness

Repeat the exact164/UMD5C74BF98/direct ICD3508416F bounded GPU Present trial after
correcting both ends of startup log sharing. Router stderr uses _wfsopen with
_SH_DENYNO and checked _dup2; reader uses ReadWrite|Delete sharing. Original secure
CRT open is the negative control. Record first observed CreateDevice UTC and
readiness latency; join actual device start from ETW after the run.

Up to30s startup, then180s measurement, stable identity and exact module hashes.
CPU image readiness, GPU Present/error checks,300s watchdog and durable rollback
remain. Registered baseline CF3948D6 is restored; exact163 rollback verified.
Hypothesis: a cooperating log writer lets the readiness reader observe actual
startup rather than abort on sharing. Missing device/modules still time out.
No G0 pass without correct content, DWM GPU ownership and no full-frame CPU copies.
