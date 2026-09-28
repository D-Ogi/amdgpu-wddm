# M687: hosted GPU client map-audit positive control

Measured on unit A, 2026-09-27. Runner source 17c0f17; control source aa0c7a8;
Mesa source 0185cb8db7eafade3b243d1f8474fb8613981407. Exact executable and DLL
hashes are in manifest.json; loaded candidate paths and hashes are in modules.json.
UMD CC82A2D9 and hosted ICD 3508416F were observed in client PID3024.
DWM remained CPU PID7204. This is not a desktop G0 acceptance run.

## Result

The client passed GPU clears, 12 deliberate full-frame CPU uploads (3,686,400
application-copy bytes), and two isolated readbacks. Both readbacks have 76,800
correct pixels and zero mismatches. Eight external checkpoints were acknowledged.
The GPU interval has three buffer maps and no image/full-frame write maps. The
CPU-copy interval contains exactly 12 full-frame write maps. Each separately
bracketed readback contains one full-frame read map. Aggregate overflow is zero.
See client-analysis.json and stdout.log for phase boundaries and map identities.

Through marker8: 24 successful maps, 16 ends, no pending requests, eight live
buffer maps. The complete teardown trace reconciles 89 sequenced events,
24 requests, 24 successes and 24 ends, with no remaining live maps. The last
checkpoint precedes the final eight ends; it is not itself a terminal checkpoint.

Seven live buffers (map IDs5/8/11/14/36/39/42) are 24,000-byte buffers with
bind0x8008000 and usage0x703. Source inspection at the exact Mesa revision identifies
ZINK_BIND_DESCRIPTOR as bit27 (zink_resource.h:31); the descriptor-buffer allocation
and persistent/coherent/read/write/thread-safe map are in zink_descriptors.c:1663-1668.
This is source-supported role classification, not a call-stack measurement. They
must not be described as query-result buffers based only on their long lifetime.

Map18's resource pointer matches the uploader map event. Its 1 MiB capacity is
not a count of CPU-written bytes: the two observed suballocations requested 4 and
16 bytes (20 total), and u_upload_data helper-copy bytes are zero. Both observed
uploader managers are destroyed. The analysis does not count later stores through
returned pointers or a manager wholly absent from the trace.

## Closure and limits

Client exit0 at17:07:43Z, worker exit0 at17:07:59Z, watchdog exit0 at17:08:01Z.
The closed receipt at17:10:51Z verifies restored UMD8279AC7F/ICDCF3948D6.
The first SSH status observation timed out; independent status commands found the
already completed run. It was not relaunched. Both trial tasks were removed.
Full raw preflight/closure and observer diagnostics remain under
scratch/g0-hosted/audit-client001-ops; raw-hashes.json inventories the pulled results.

stderr.log contains only verbatim BC250 audit lines selected from the raw log;
unrelated diagnostics are omitted. Other copied receipts and stdout are unchanged.
All three analyzers can reproduce their results from this directory. The regression
control rejects wrong copy bytes, a pixel mismatch, a missing final marker and a
wrong loaded module hash. No new lab execution was needed for those mutations.

This validates detection of the deliberately introduced map-based copies in this
client. It does not prove absence of every CPU store, desktop correctness, DWM-owned
GPU work in this run, or full G0. Next: carry these checkpoint boundaries and role
accounting into the bounded desktop run, including capture-readback separation.
