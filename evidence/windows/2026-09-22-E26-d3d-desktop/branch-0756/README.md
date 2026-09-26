# Conditional branch targets, 2026-09-23

KMD0.7.56.1 unchanged. Mesa branch-labels UMD SHA256440B2AB98A943E3F77849D5C1672271792C71F5C864D4BB1562E27B3C2DFC43F. Test mesa-branch-probe.ps1, restored01:37:42, task result0. See experiments/E26-wddm-desktop/mesa-branch-labels.patch and tgsi-branch-test.c.

Fresh DWM2284 log has556 DrawIndexed entries and19 Present calls, all19 Present callbacks return S_OK. Snapshot samples active shader evaluation, not the previous assertion dialog. This demonstrates progress beyond the old blocked draw; it does not certify all shaders or presentation correctness. Final scanout is1920x1200 and every RGB pixel is black. Shared two-device red/blue and green staging controls report zero mismatches. The capture contains a DWM restart event; its cause is not established here. These results do not meet an accelerated desktop criterion.

Finally restored display-only, restarted DWM and confirmed UnconfirmedStarts0. Raw process dump remains outside the repository. No GPU engine bring-up in this test; softpipe executes on the CPU.
