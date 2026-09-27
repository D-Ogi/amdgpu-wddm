# M624: KMD156 built and staged, withheld before execution

Isolated source cf1aabb8121079cf86fca098214f1978f9624f5d adds ede9a15 and
acf9c4c to155/19f0dfe.303-file baseline audit remains bounded to the same six
existing driver files. Full build passes12 gates, compile/link/stack/catalog/
signing and clean-source identity. SYS SHA256:
309182C1C9AA77314D4589EFE8C75ED7B7F79DC3FDE59A1A06CF9E205AF9ED55.

Read-only lab preflight at06:57:30Z confirmed153/0C0DA4E1, STOP false, no test
processes, only overlay/watchdog tasks and66C. Private full receipt remains in
scratch/g0-hosted/kmd156-transition. Candidate and rollback153 were staged and
all hashes plus target PowerShell5 syntax checked. The task was Ready, not run.

Review identified an unresolved lifetime case: AcquireHandleData could return
NULL private data with a nonzero release handle. This artifact releases only
on non-NULL private data. It must not run the probe until that case is handled.
No worker-start receipt existed; Cleanup removed the unstarted task at07:02:11Z.
No driver install, registry gate change or lab transition occurred.153 remains
loaded. Staged files are retained as evidence, not an approved runtime candidate.

Build/staging/cleanup logs are copied unchanged. No private identifiers removed.
