# M696 - bounded store-audit client004

Measured on unit A, 2026-09-28 local. Runner3cb4f5a, parser019fe24,
KMD166/1798984, hosted ICD C0CE, Mesa3484e1f4.
UMD SHA256 B514FF61A0EEAD7E3F7D6225B51AE42AD01D4C9BDA571E9EC7C4CFB446B4C01D.

Both76800-pixel readbacks pass with zero mismatches. Twelve deliberate
UpdateSubresource uploads and eight checkpoints pass. Full-log lifetime/store
and DDI parsers pass. Two zink_buffer_subdata startup copies are measured:
4 bytes at backing offset0 and16 bytes at4096, both mapped resource16. Each
has a completed pair before marker1; no pending spans or boundary crossings.

No descriptor stores executed in this clear/copy client. It cannot validate
their runtime instrumentation or log rate. stderr is50146 bytes with four store
records. A texture draw control is required next; G0 and full writer coverage
remain open. This does not retroactively attribute stores in DWM042.

One observer timed out. Independent heartbeat and saved receipts established
normal completion, without relaunch or reset. Client/watchdog exit0, baseline
UMD8279/ICDCF39 restored, tasks removed, CPU DWM and boot retained. Health15,
1000MHz/VID116,67.1C. Full receipts retained locally in audit-client004-ops.

store-checkpoints.log contains only the reviewed store/checkpoint lines, with
numeric counters and internal sequential IDs. stdout.log is the unmodified
application result. No pointer-bearing map/DDI logs, system identifiers or
configuration were exported. summary.json is derived numerical analysis.
The full local log is required to rerun the parser; this selection is not a
complete sequence and must not be passed off as one.
