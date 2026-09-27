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
