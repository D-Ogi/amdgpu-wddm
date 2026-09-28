# Startup log without a console (M670)

Host controls, 2026-09-27; DWM032 prepared, not staged or run on unit A.
The Windows-subsystem control starts with DETACHED_PROCESS, no inherited standard
handles and close_fds. It records _fileno(stderr) before and after the helper.
The exact DWM031 helper begins at -2, rejects redirect and leaves -2 with no readable
marker. The new helper starts at -2, returns success, produces descriptor3 and lets
a live shared reader retrieve the marker. Both results come from receipt files,
not from console output. A test invalid-parameter handler prevents default fail-fast;
it is called zero times in both observed cases. The first negative-test assertion
incorrectly required a handler call; the corrected test requires the actual failed
redirect. The first receipt remains private, unchanged.

The new helper uses non-secure _wfreopen to reinitialize the FILE object with shared
access. A narrow C4996 suppression documents this intentional sharing choice. The
actual old secure-open sharing negative and new live-reader/second-logger positive
also pass. No production UMD, ICD or KMD change.

Router/control/host controls compile with /W4 /WX. Prepared scripts parse in host
Windows PowerShell5.1.26100.9444. The final router adds precise FILETIME/QPC/PID/TID
records bracketing the original CreateDevice call; this is separate from the
sampler observation time. Runtime artifact hashes retained in the20-file manifest.

DWM032 keeps the identity ambiguity/replacement rejection,30s readiness,180s measured
interval,300s watchdog, exact163 rollback and registered baseline CF3948D6. It retains
UMD5C74BF98/direct ICD3508416F to separate the stream fix from adapter-enumeration
changes. Target validation, actual CreateDevice success, image content, DWM GPU
ownership and exclusion of full-frame CPU copies remain required. No G0 pass.
