# DWM030: startup reader sharing failure, not a measured desktop interval (M667)

Unit A, 2026-09-27, runner eb595df. The 20-file staging manifest and target PS5
parse/no-marker restore controls pass. Exact KMD164/9B9B99D3, baseline UMD8279AC7F
and registered ICDCF3948D6 verified before the one launch.

The first startup sample throws IOException: ReadAllText cannot open dwm-9720.log
while the UMD holds it for writing. The runner exits before recording a ready
receipt or starting the measured animation: zero measured seconds. Automatic
restore completes at 12:23:37Z and terminal runner receipt at 12:23:39Z.
A retained early summary contains 9 BGP1 submissions, 9 actual node0 completions,
zero rejected/failed/timeouts/refusals. This proves neither image contents nor G0.
The retained UMD log also has CreateDevice 8007000e/failed-to-choose-pdev and device
lost teardown messages. Their ordering against rollback is not resolved here;
they must not be silently interpreted as successful device initialization.

Collector8804/start12:23:22.3487097Z finishes at12:26:23.0800301Z with159 samples,
no reader timeout. Archive and cleanup run only after matching process absence
and the terminal receipt. All030 tasks removed. Fresh12:27:35Z closure verifies
CPU3924, both baseline hashes, exact164, health15/guard0,1000MHz/66.875C, same boot.
Registry and latched interop/GPU Present gates0 are checked by cleanup.
Full ETW/archive remain private; hashes retained here. Raw logs were not modified;
startup-selected.txt contains only selected counter lines from the identified log.

The shared reader now opens with FileShare.ReadWrite|Delete. A host control holds
a real writer open, requires old ReadAllText to fail, then passes the actual wrapper.
Additional controls cover a real missing-process error and transient missing modules;
all retry and reach readiness. Seven core controls still pass. First observed
successful CreateDevice UTC is preserved separately from final readiness. These are
host controls, not a runtime pass for the updated reader. Original030 artifacts
remain frozen. G0 remains open.
