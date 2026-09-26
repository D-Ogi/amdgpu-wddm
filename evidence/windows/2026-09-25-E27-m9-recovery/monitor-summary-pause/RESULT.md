# Monitor LOG_SUMMARY pause control

Source identified: GraphicsPipelineProvider.Period is five seconds. Its Poll
calls KmdInfoProvider.Run(CliPath, "log summary", 5000, ...). CliPath resolves to
C:\BC250\kmd\bc250kmd_cli.exe. ProviderHost sleeps for Period after Poll, explaining
the observed approximately 5.056-second interval (poll duration plus five seconds).

Added an opt-in marker in the monitor's existing data directory:
graphics-summary.pause. Default behavior is unchanged when the marker is absent.
The marker suppresses only that LOG_SUMMARY subprocess on subsequent provider polls.
An already running call is allowed to finish. Removing the marker resumes sampling.
DWM/module inspection, overlay, KmdProvider typed health, KmdInfoProvider info and
native clock/temperature providers remain unchanged.

Paused graphics counters use the last successful summary with an explicit
"paused; snapshot HH:mm:ss" row. Pausing before any successful summary shows
"paused; no cached snapshot" and makes no counter claim.

No lab access, deployment or live configuration changes were performed.

## Build and tests

Production files changed: tools/win/bc250mon/src/GraphicsPipelineProvider.cs only.
New tests: test/GraphicsSummaryTest.cs and test-graphics-summary.ps1.

- Actual provider summary boundary with stub subprocess: 14 checks, zero failures.
  Default call, no subprocess while paused, repeated/cold pause, unchanged cached
  values, stale indication, dynamic resume/fresh counters and normal failure path.
  No UI, DWM inspection, GPU access, host registry or native DLL calls in this test.
- Existing production build: 6 Python tests, 34 inventory checks and 120 typed
  health policy/provider checks pass; full C# build succeeds with warnings as errors.
- Initial Windows PowerShell 5.1 build wrapper stopped on Python unittest stderr;
  rerun using pwsh succeeded without modifying the production build script.

Artifact: scratch/build/monitor-summary-pause/bc250mon.exe
SHA256 E75DDCC8AADFED045BC4D453054917922976E07698F0F12486FF21ED1EA2D8AE
Bundled existing146 bc250control.dll:
B0C7819FC30FDD9F73BD0523ACAA0AFA7BBDF7D5A83A2E22D40B9949AC38AFA3

## Remote toggle after a separately authorized deployment

For the default monitor data directory, create the marker through target.py ps:

    [IO.File]::WriteAllText('C:\BC250\mon\graphics-summary.pause', 'S4 A/B diagnostic pause')

Allow the existing in-flight summary (up to five-second subprocess timeout) and
next provider period to complete. Confirm the overlay graphics panel indicates
paused counters. This is not the owner STOP flag; the other providers keep running.

Resume normal summary polling:

    Remove-Item -LiteralPath 'C:\BC250\mon\graphics-summary.pause'

If bc250mon was started with a custom data directory, use that directory instead.
No monitor restart, environment/registry setting or driver change is needed to
switch modes. The marker persists across S4 and restart until explicitly removed.
No marker was added by this work, so default operation remains enabled.

This supplies the A/B control; it does not establish that LOG_SUMMARY caused the
reported S4 cursor stalls or subsequent SSH hang.
