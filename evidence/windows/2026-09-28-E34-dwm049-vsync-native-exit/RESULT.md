# M719 - DWM049 VSync and image controls pass; native exit receipt fails

Runner a338182, package C68F0119413995A00B2C8924FD08716FACED2C16E4CF62B284454401DF788DC2. Exact169/AF715A56, hosted UMD92697AE5/ICDC0CE. [Decoded selected receipts](observations.json); raw logs, captures and ETL in scratch/g0-hosted/dwm049-ops outside the repo. No raw device identifiers exported.

Trial125.230s ends success=false after the final native-client completion check: client-done.json contains exit=null. The client stdout reports PASS2837 frames/frozen1/no_readback1, but that text does not replace a process exit code or establish no CPU copies across the desktop. No complete trial acceptance is claimed.

Both frozen image checks pass40712 pixels with no mismatch. The VSync interval87.381s has5238 ACKs,5237 reports (59.944Hz), deferred3/old-buffer2, recovered DPC ACK8 and no read/ACK/sync error delta. Final read-begin ACK and notify ages are-14.011ms (events during the read). Start/end KMD TDR summaries are clean/CollectDbgInfo0. Boot unchanged and immediate event query empty; asynchronous WER can appear later. Snapshots do not establish per-frame ETW continuity/retirement.

CPU169/UMD8279/ICDCF39 restored, tasks removed, collector/watchdog terminal. Independent preflight confirms health15 generation59886818448/epoch5, same OS boot. See receipt for timestamp and temperature. No reboot/power cycle. KMD166 rollback package retained.

Native wrapper correction retains Process.Handle before waiting and reads GetExitCodeProcess instead of trusting nullable PS5 Start-Process ExitCode. Real child exit0 and exit7 controls pass on the host. That correction has not been exercised by another GPU trial; original049 staged files and evidence are preserved.

Next: offline ETW flip/VSync and DWM-execution attribution, then new trial identity with fixed exit receipt. Full no-copy and remaining M13/G0 acceptance stay open.
