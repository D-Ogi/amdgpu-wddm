# M480: release CTS basic compute control

Unit A, 2026-09-25. Release vulkan-cts-1.4.6.2,
commit f6a29701220f34dd1407513bfe80d74ca7b392ce.
Binary SHA256: ae7befdd190ef08e4a715de0348734879263e1854e017d67865749905a95a2b6.

## Result
All80cases of compute.pipeline.basic from this release's must-pass compute list
completed:75Pass,5NotSupported,0Fail. QPA, per-case ledger, native exits and
actual loader/ICD module paths agree. This is a selected development control
from the acceptance release, not full must-pass or a conformance submission.

The five unsupported cases are concurrent_compute, secondary_compute_only_queue
and the three replicated_composites_coopmat variants. They are the same named
unsupported cases as M479. Matching Linux behavior remains unmeasured.

Start11:53:40Z, finish11:54:15Z; same Windows boot11:20:11Z.
KMD147, desktop UMD8279AC7F, ICD9C40083C/Mesa f333dd6d and system
loader5C42CA8E remain unchanged. Five recorded input hashes match at final
readback. Sampled clocks stay1000MHz/VID116; 8 inter-case health
samples keep flags15/generation590515602/epoch5, maximum sampled temperature
67.375C. These are discrete samples, not a continuous maximum.
Independent final readback:66.500C, no selected new fault events or dumps.

## Harness compatibility
The first release launch stopped before any test because the limited token
could not use the KMD clock escape (STATUS_ACCESS_DENIED). The worker now uses
the lab's elevated interactive token and records it. M479 separately covers
normal-user Vulkan discovery. No system security policy was changed.

The next launch rejected main-only watchdog interval/total command-line options
before loading the ICD. Its raw output and final health readback are preserved
under cli-rejection in the archive. The final worker uses the release's supported
watchdog flag plus its own45second external process deadline, stopping the series
at the first failure. No Windows, DWM or GPU restart occurred between these
release attempts. Registration uses no ICD environment override.

## Reproduction and limits
E33 compute-release-basic.txt selects the entire basic group, placing the known
copy_ssbo_single_invocation positive control first. Source, eight fetched
dependency commits, must-pass file hashes, build command and binary hash are
in raw-controls.zip. The build has video tests disabled and is not an unqualified
full-conformance build. Full corpus selection and platform differences remain
part of M12 acceptance.

raw-controls.zip contains every successful run file and the CLI rejection.
Only adapter LUID values are redacted; manifest.json records both byte hashes.
audit.json records the independent comparisons. No failed/unsupported case is
counted as a pass. No performance, sparse, OpenGL, OpenCL or D3D acceptance is
inferred.
