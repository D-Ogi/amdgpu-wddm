# Paging queue storage reserved by builders

Plan2026-09-24, written before candidate133 hardware deployment/testing.

Hypothesis: reserving an independent job slot for every supported submission
start in the OS DMA-private buffer removes nonpaged allocation/copy from
WddmSubmitPagingHardware while preserving bytes, progress, FIFO and lifetime.
M424-M425 prepared the format and consumer. This change migrates all builders,
including update/copy/fill/aperture/captured/native paths, and removes legacy
heap fallback from queue admission. Both actual callers require nonempty ranges.

Host acceptance: actual builders reserve queue bytes before logical publication;
DMA/private budgets and multipass stay consistent. All produced records expose
zeroed slots. Actual direct/native output is imported by the actual queue test.
No pool allocator mock is linked into that queue test. Real completion releases
ownership before a mock OS overwrites the slot; reversed-order mutation fails.
Native private capacity127 is insufficient and cannot publish; direct one-byte
short publication and existing DMA/private/live-ring limits are checked.

Hardware procedure: preserve M423132 logs and the M412D3D/M414RADV modules.
Use one PnP disable/install/enable transition to version0.7.133.1, no routine
OS/DWM/AC reset. Confirm STOP clear,1000MHz/820mV,temperature below85C and exact
candidate/module identities. Start with existing engine controls and64MiB
three-cycle/four-readback residency. Then8shader CPU oracles, two complete E14
model output comparisons, and1GiB three-cycle/four-readback if healthy. Record
queue admission counter, native/capture/fence/error counters before/after.
Require native exits0, correct full GPU content, honest fences and noTDR.

On a stalled observation, inspect that existing worker/SSH state before any
recovery; preserve raw logs. DWM recovery only if appropriate, PnP or AC only
if needed. Retain132 rollback. This does not establish forced preemption,
OS cancellation/generation, PFN/cache aliases, cold/power startup or matched
Windows/Linux performance. No resource guarantee is claimed for capture fallback.

Results and installed identity will be recorded after the run.

M426 result: candidate133 passes all stated host, shader/model and64MiB/1GiB
content controls,30673 borrowed admissions/completions, noOS/DWM/ACrestart.
See [source, raw records and limitations](../../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-os-private-queue/RESULT.md).
