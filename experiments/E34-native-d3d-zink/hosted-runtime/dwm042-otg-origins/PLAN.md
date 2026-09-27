# DWM042: desktop DDI attribution with OTG timing samples

Prepared, not run. Preserve DWM041 timing/recovery: shared pre-mutation QPC,
render stop105s, final markers130s, watchdog rollback140s, total acceptance180s.
The same composition control and eight requested phase markers are retained.

Use UMD49A44067 from Mesa13e623af and hostedICDC0CE5DCD with paging-fence fix.
M690/691 validate Map and UpdateSubresource attribution on this exact pair.
Baseline KMD166/UMD8279/registeredICDCF39; check it fresh before mutation.
M694 validates this exact KMD on CPU with OTG sampling and native GPU copies.
It does not establish desktop stability. DWM041 on KMD165 ended in0x116 (M693).

Hypothesis: render-phase image writes can be attributed to concrete DDI calls,
including any desktop-sized map. Preserve unmatched/internal maps; do not infer
no-copy from geometry alone. Require DDI scope consistency and copy witnesses,
correct selected pixels, loss-free DWM-owned DMA and fence progress. Persistent
pointer writers and initial-data coverage remain separate requirements.

The startup collector already requests log summary and durably saves each raw
result. KMD166 adds OTG begin/end time, valid mask, frame count, sync status,
control, master lock and position. Select the newest complete snapshot in each
returned ring log, not the first historical record. Compare actual begin-time
intervals and modular frame deltas against VSync/IRQ progress. Nominal polling
cadence is not elapsed time. Sequential reads are not an atomic hardware snapshot.
If timing stops, distinguish it from continuing frames with stale VSync reports;
a single status level or a final counter alone does not establish either case.

Build router/control /W4 /WX, parse scripts underPS5 on target, package exact
hashes, verify target staging, announce overlay, launch once and observe to
terminal. Host controls cover seven watchdog decisions, five DLL interruption
boundaries, durable helpers and syntax; they do not simulate OS/GPU hangs.
Archive receipts, maps/DDI/ETW/images; verify rollback and remove terminal tasks.
No repeated launch on observer timeout. No prior desktop result is inherited.

Host preparation: build and controls pass. Package manifest
85B2EEBB03E4C9882EC2D71C8F942E976C5D4E80FBACFE2DFC8DA9BBAEE11900.
No target staging, driver change or desktop run in this preparation.