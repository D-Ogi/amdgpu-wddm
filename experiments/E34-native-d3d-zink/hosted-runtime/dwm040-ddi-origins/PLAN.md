# DWM040: DDI attribution for desktop image writes

Prepared, not run. Preserve DWM039 timing/recovery: shared pre-mutation QPC,
render stop105s, final markers130s, watchdog rollback140s, total acceptance180s.
The same composition control and eight requested phase markers are retained.

Use UMD49A44067 from Mesa13e623af and hostedICDC0CE5DCD with paging-fence fix.
M690/691 validate Map and UpdateSubresource attribution on this exact pair.
Baseline remains KMD164/UMD8279/registeredICDCF39; check it fresh before mutation.

Hypothesis: render-phase image writes can be attributed to concrete DDI calls,
including any desktop-sized map. Preserve unmatched/internal maps; do not infer
no-copy from geometry alone. Require DDI scope consistency and copy witnesses,
correct selected pixels, loss-free DWM-owned DMA and fence progress. Persistent
pointer writers and initial-data coverage remain separate requirements.

Build router/control /W4 /WX, parse scripts underPS5, package exact hashes,
verify target staging, announce overlay, launch once and observe to terminal.
Archive all receipts, maps/DDI/ETW/images; verify rollback and remove terminal
tasks. No repeated launch on observer timeout. No prior result is inherited.
