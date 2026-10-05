# GPU operator control, run003

UnitA boot02:18:54, KMD0.7.56.1. The b9564 test-backend-ops harness loads the unchanged release Vulkan plugin and exercises Vulkan0 (RADV GFX1013).75/75 cases pass:64 F32 CPY cases and11 F32 RMS_NORM cases, process exit0. The selected parameter regex matched no MUL_MAT or SOFT_MAX cases; no coverage of those operators is claimed. First verify corrected selectors on CPU before the next GPU run. Display-only restored, UnconfirmedStarts0 at02:23:45.

This narrows the inference failure but does not validate all data transfers, precisions, shapes, graph dependencies or model inference.
