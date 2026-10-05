# M587: per-draw completion diagnostic

DWM017 ran185.815s on unit A with diagnostic UMD
7C1554FD84DDF53B155D5813D09920E30C1BA52E0836BE01455AD38C89012D09
and hosted ICD3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71.
BC250_HOST_DIAG_DRAW_IDLE=1 flushes and waits after each frontend draw.
The first three logged waits completed. This is a diagnostic, not a production fix.

Control108 passes56 state images/229376 pixels/3584 draws. DWM017 has41826
matched DMA pairs attributed to DWM10324, zero trace loss, no unmatched/pending
or duplicate starts; submission/completion IDs agree, no preemptions.
Static8000-pixel composition ROIs and all sampled cyan shape checks pass.

The owner initially reported a correct image, then reported a slight artifact
in the newly exposed area when shrinking the blue/cyan window. Preserve both
observations: this is NOT full visual acceptance. Sparse image checks do not
exclude this transient defect. Large stretched geometry was not reported.

This run changes both flush frequency and GPU retirement timing. It cannot
identify which change suppresses the earlier severe corruption. Next control:
flush each draw without a CPU completion wait. No claim of complete G0,
performance acceptance or new exclusion of CPU frame copies is made.

Automatic rollback restored baseline ICD93B1D1FD and CPU UMD8279AC7F,
verified by cleanup; CPU DWM2372, all017 tasks removed. Raw ETW, images and logs
remain private; hashes and derived measurements are retained here. No promotion.

PROVENANCE: Mesa (https://gitlab.freedesktop.org/mesa/mesa), MIT.
