# Requested map audit checkpoints

BC250_HOST_AUDIT=1 enables the instrumented Zink path. Set BC250_AUDIT_MARKER to
a runner-owned absolute file path before starting the process. Leave the file
absent until the first request. The path is read once from the environment; the
small file is then read at each instrumented Flush. This diagnostic file polling
and serialized logging are not suitable for performance acceptance.

Write a positive decimal uint64, optionally followed by ASCII whitespace. Publish
through an atomic replacement in the same directory. One runner owns the file;
only one outstanding request at a time. Use increasing IDs within this process.
Empty, malformed, overflowed, zero and obsolete IDs receive no acknowledgement.
There is no command execution or memory write operation encoded in this file.

Wait with a bounded deadline for the matching `event=checkpoint marker=N` in the
same process's log. The actual field order includes sequence/time before marker.
The acknowledgement is emitted under the same lock as all lifetime events, has
cumulative counters and calls fflush(stderr). Marker observation forces the usual
context aggregate snapshot too; that following snapshot is a separate record.
A missing acknowledgement invalidates the requested measurement boundary. An idle
process may need new ordinary rendering/Flush work to observe the request.

Acknowledge START, run the measured workload, acknowledge END before DWM rollback.
Retain the full event prefix from process start so all IDs, sequences and counters
can be reconciled. Do not merge multiple process or DLL-load instances. Parse:

```powershell
python analyze-map-lifetimes.py dwm-audit.log --start-marker 1 --end-marker 2 --allow-live
```

`--allow-live` reports surviving transfers rather than proving them harmless.
Pending map requests still reject the terminal boundary. Classify every live
mapping, especially persistent image/user pointers, before no-copy acceptance.
The parser stops at the requested END and validates all preceding checkpoints;
START/END counters produce interval deltas. An explicit required END is necessary
because an arbitrary log tail may be truncated after a whole completed map.

The checkpoint orders audit operations on the CPU. It is not a GPU completion
fence, does not stop other contexts, and does not count later stores through a
returned pointer. Pair it with rendering/Present fences, content controls,
uploader events, capture brackets and CPU-copy positive controls. Capture BEGIN
and END markers can use the same strictly increasing sequence of request IDs.

Host controls use the retained M684/M686 event fixtures with test-map-lifetimes.py
and test-map-checkpoints.py. Legacy unsequenced logs remain readable but cannot
satisfy a requested checkpoint boundary.

## PowerShell runner helper

`request-audit-checkpoint.ps1` exposes `Request-AuditCheckpoint` for Windows
PowerShell 5.1. The caller supplies the exact DWM PID/start time, marker/log paths,
a running trial stopwatch, a shared absolute elapsed-time deadline, and increasing
marker IDs. It opens the log before atomic marker publication, tails complete lines,
and returns the matching acknowledgement plus process/time metadata. A pending map,
changed process identity, missing acknowledgement or expired deadline rejects the
boundary. Diagnostic lines are capped at16 KiB and observed bytes at8 MiB per call.
The full saved log must still pass the sequence/lifetime analyzer after collection.
This helper alone does not enforce rollback or prove CPU-store coverage.

`test-request-audit-checkpoint.ps1` runs without the lab. It checks a deliberately
split acknowledgement line and rejects a repeated marker, pending map, missing
acknowledgement, expired trial deadline and changed process start time. Windows
PowerShell 5.1 requires `[NullString]::Value` for File.Replace's absent backup path.

Desktop integration remains pending: set BC250_AUDIT_MARKER and BC250_UPLOAD_AUDIT
before loading the candidate; bracket each capture separately; retain full process
logs. Start the shared trial clock before the GPU transition, reserve rollback time
inside the owner's180-second budget, and give the independent watchdog the same
boundary. Do not reuse DWM038's160-second post-startup loop and300-second watchdog
as if they implemented an overall three-minute limit.
