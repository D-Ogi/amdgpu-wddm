# Bounded DWM hosted-device probes

M547, unit A, 2026-09-26. Same tested candidate016 as M546, ICD3508416F
and UMDC95C6D32. These probes do not pass G0.

Probe001 loaded the candidate in DWM1300, but no hosted CreateDevice diagnostics
appeared. Probe002 added typed WDK adapter callbacks in the router: DWM6264
queried three D3D10 versions and caps130, unloaded/reloaded the adapter, and
created a device without hosted initialization. The one-attempt claim guard
incorrectly rejected reloads in the same process, selecting the CPU backup.
This was a harness flaw, not evidence that DWM rejects D3D10 or hosted Zink.
Probe003 accepts an existing claim only when its stored PID matches the current
process, retaining the candidate across reloads. Another process still falls back.

Probe003 DWM7904 calls CreateDevice interface000a0009/version0000177a. Hosted
initialization completes, with runtime paging/context/sync/allocation/VA/residency
callbacks and CreateDevice returning S_OK. Module snapshots bind the candidate
files. Further allocations appear, but there is no recorded hosted SubmitCommand
or native Present in this interval. The screenshot request times out; the next
STOP-endpoint query also times out and causes the runner to fail. No correct
composed image, GPU composition completion or no-copy conclusion is supported.
Next: bounded DDI entry/exit tracing to locate the post-CreateDevice stall.

Each main finally restores baseline libraries and restarts DWM. Separate30-second
SYSTEM tasks verify restoration independently. After003, DWM2956 loads baseline
UMD8279AC7F; the registered ICD is9C40083C, overlay flags responds stop=false,
and the watchdog is terminal. No KMD update or requested OS reboot. A DWM restart
is not evidence of a GPU reset. The initial and subsequent CPU DWM PIDs are
recorded in transcripts and restored.json for each probe.

The router has a30-second enable lifetime and one-PID claim; this is a bounded
lab harness, not a production loader. Scripts verify hashes, back up libraries,
verify a running watchdog before mutation, and serialize rollback with a mutex.
The router builds with /W4 /WX; the WDK anonymous-struct warning is suppressed
only around its includes in002/003. DDI tracing does not change advertised caps.

Raw full-screen captures remain outside Git; hashes are included. No image
correctness claim is made for these probes. Transcript Username, RunAs User and
Machine fields are redacted; private originals remain in scratch. Other logs
are retained unchanged. Candidate manifests identify binaries; exact router and
runner sources accompany each probe. Source patches for the candidate are in
E34 hosted-runtime through lifetime-*.patch.
PROVENANCE: Mesa MIT; Microsoft WDK10.0.26100 DDI declarations.
