# DWM043 - bounded descriptor and staging store census

Prepared, not run. Preserve DWM042 recovery/time boundaries: shared pre-mutation
QPC, render stop105s, final markers130s, watchdog rollback140s, acceptance180s.
Exact166/1798984 remains installed. UMD3484e1f4/B514FF61 replaces49A44067;
hosted ICD C0CE stays fixed. M696/697 validate completed startup subdata copies,
texture descriptors, negative full-frame-copy control and checkpoint counters.

Hypothesis: DWM persistent descriptor writes are bounded completed spans, and
uploader-backed writes can be joined to their backing resources and phases.
Require all8 markers, reconciled store/map sequences and counters, exact loaded
module identities, expected selected pixels, loss-free DWM-owned DMA pairs and
fence progress. Unknown/pending writers remain unresolved; do not treat all
image uploads as final-desktop copies or infer full coverage from absence alone.

Source audit and runtime must distinguish descriptors, initial/subresource input
uploads, application ResourceMap writers and final presentation surfaces. Capture
phases stay separate. Log per-op volume/rate; no performance claim from this audit.
Use newest complete OTG snapshots as in042, preserving non-atomic-read limits.

Fresh preflight/STOP/temperature/overlay precede launch once. /W4 /WX router and
composition builds, watchdog/interruption/durable tests, target PS5 parsing and
manifest validation precede mutation. Restore CPU baseline8279/CF39/gates0,
remove only terminal tasks and archive evidence. Observer timeout is not a
relaunch reason. G0 does not follow from a small client or this plan.
