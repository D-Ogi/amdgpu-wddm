# M699 - DWM044 marker was produced after its deadline

Exact KMD166, UMD3484e1f4/B514FF61 and hosted ICD C0CE; runner1e11699.
The attempt aborted after25.9418905s on its first checkpoint acknowledgement.
The measured render interval is zero. There is no accepted pixel/G0 result.

The request was published at QPC79880301738, frequency10000000 from the router
log. Mesa3484 src/util/os_time.c:57 delegates to src/c11/impl/time.c:70-85,
which scales the same monotonic QPC into nanoseconds. The marker timestamp
7992868729200 is4.8385554s after publication, outside the4s deadline.
The reader consumed240824 characters starting at40839; the last preceding
complete sequenced record at byte281590 has time_ns7992029422700, or3.9992489s
after publication. A73-character partial line remained. Thus marker production
was late; this result does not establish that logging was its sole cause.

Rollback succeeded; tasks were terminal and removed, original collector terminal
with22 samples. Final22:46:49Z September27: exact166, baseline8279/CF39 restored,
health15,1000MHz/VID116,67C, no trial processes, same OS boot. CPU DWM2688.
Receipts remain in scratch/g0-hosted/dwm044-ops/result; ETL preserved on lab.
Export contains only the numeric marker and reduced results; unrelated metadata
is omitted. No source evidence was modified. No new GPU run was started.
