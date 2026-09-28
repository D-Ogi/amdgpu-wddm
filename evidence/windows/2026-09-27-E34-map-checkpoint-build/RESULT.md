# M686: serialized map audit checkpoints

PROVENANCE: Mesa fork (MIT), source 0185cb8db7eafade3b243d1f8474fb8613981407, parent 150631a8584a5c8c8ae676b09820561d6317438d.
UMD SHA256 `CC82A2D9BAD5534D54B1460B1FF49A1F83ED40CADF8694A5F7C1F87CB02386FD`, 15182336 bytes.
Full target build passes. Not deployed; no new GPU or performance measurement.

Every begin/result/end event now takes the process/DLL-local lifetime lock and
receives a consecutive event sequence. Requests, successes, failures and ends are
counted under the same lock. Checkpoints contain these counts, pending requests
and live transfers. Existing sampled Flush snapshots emit checkpoints; an optional
BC250_AUDIT_MARKER file allows the runner to request a numbered checkpoint at the
next Flush, also forcing the context's aggregate snapshot. stderr is flushed at
the acknowledgement. This is a CPU audit boundary, not a GPU fence or global
quiescence guarantee. Other threads may have pending/live transfers, explicitly
reported. Counters/sequence are process-wide within this loaded UMD, not per device.

The actual ID/event/marker helpers were extracted unchanged and compiled with the
real Mesa atomic/simple_mtx headers and its built Windows futex backend. Four
threads produce800 requests,400 successes,400 failures and400 ends between
markers11 and12. A further successful map is deliberately live at99 and closed
at100. Final totals801/401/400/401 reconcile. Marker parser controls reject six
invalid/obsolete requests. Resource metadata, environment option and clock are
stubs; the counter/log synchronization and file parser are actual code. This is
not a GPU map-path integration control.

The verifier rejects11 mutations, including deletion of an entire map and deletion
followed by event resequencing (caught independently by checkpoint totals), missing
boundaries, forged counters and unpermitted live maps. Normal and Python -O
outputs agree. The original M684 fixture still passes in legacy mode, which does
not gain checkpoint coverage. Source helpers were compared byte-for-byte with
the committed source after the user interruption; completed results were preserved,
not rerun on the assumption that interruption meant the test had failed.

Initial standalone harness builds needed the SDK timespec define, /MD and Mesa's
futex/time objects plus Synchronization.lib. The final successful recipe and log
are retained here; failed preparation logs remain under private scratch. Existing
build.cmd records the original17-step build. build-low-memory.cmd is the future
-j2 recipe, prepared after a host-memory concern (the owner later attributed that
pressure to another agent). No resource-intensive work is running from this control.

Remaining: deploy with a positive copying control, correlate uploader identity and
capture brackets, inspect persistent mappings and imported-primary CPU access.
A checkpoint verifies instrumented events through its sequence; it cannot prove
that uninstrumented code never stores to a mapped pointer. Full G0 stays open.
