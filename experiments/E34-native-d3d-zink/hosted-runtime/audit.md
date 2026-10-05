# Bounded G0 audit counters

Hypothesis: cumulative Zink image-map counters detect the known readback maps
in the VertexID control, while monitored fence snapshots report submitted and
completed GPU values in the native Present control.

PROVENANCE: Mesa, MIT.

Set BC250_HOST_AUDIT=1 before device creation. Image and buffer counters count
map requests and their logical byte ranges, including failed requests; persistent
requests have a separate count. Counters are cumulative per Gallium context and
are sampled on the first8 flushes, each64th flush, and context destruction.
They are not counts of actual copied bytes. Context operations follow Gallium's
existing serialization requirements. The final snapshot precedes teardown.

Present snapshots include PID, runtime device, context token, synchronization
handle, submitted value and the currently observed monitored-fence value. These
are user-mode observations and still require independent GPU trace correlation.
The first8 Presents and each60th Present are retained. Existing unconditional
draw diagnostics now use the explicit verbose switch instead of per-draw stderr.

Positive controls: repeat exact VertexID readback, expected3 image maps of48
logical bytes each, then the native flip control with changing images and GPU
fences. Keep CPU DWM, bounded process deadlines and baseline hash restoration.
Only then enable the same instrumentation in a bounded DWM probe.

No-copy acceptance additionally requires an audit of imported/persistent image
mappings, KMD blit/seed counters, controlled visible image plus independent
primary readback, and independent DWM execution attribution. These counters alone
do not establish G0. No KMD changes in this candidate.

Results: [M559](../../../evidence/windows/2026-09-27-E34-audit-controls/RESULT.md).
Both positive controls pass; next apply the diagnostics to bounded DWM.
## Bounded DWM009 composition and audit probe

Candidate UMD897A38A7/ICD3508416F, router and control hashes in its manifest.
Extend DWM008 with the M559 validated counters. Two small GDI-only topmost windows
(red background and half-alpha blue overlap) plus a16x16 changing marker provide
a composition stimulus without a GPU-rendered client. The control exits after75s.

Take a baseline primary dump with CPU DWM, then trace DxgKrnl while routing only
one DWM PID to hosted Zink. Six5-second samples retain process/module identities.
KMD summaries at the first/last sample bound a steady interval; primary and screen
captures occur after that interval. Compare interior ROIs against CPU composition.
Capture output remains private until cropped or reduced to nonidentifying metrics.

Main finally restores libraries and DWM. Independent60-second watchdog restores
them even if the runner fails; enable-file lifetime and single-PID claim prevent
repeated GPU DWM crash loops. Verify hash checks and STOP/temp before launch.
A failed oracle, loss, timeout or unexplained CPU-copy counter increase rejects
G0 acceptance; preserve artifacts and return to CPU baseline. ETW submissions
alone do not establish execution or completion attribution.
