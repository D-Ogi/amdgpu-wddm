# M724 - G0 implementation completion audit

This is an offline acceptance audit of M723, not a new lab trial. [Executable checks](audit-g0.py), [result](audit-g0.json). Eight artifact hashes are rechecked against the retained inputs; twelve live DWM samples match the exact hosted modules. Whole-trial success/CPU restoration,180-second bound, image results, hardware DMA ownership/completion, exact Present/fence joins, map/store census, no-TDR witnesses and final CPU health all pass.

G0, the implementation objective of actual GPU desktop rendering through the hosted runtime path, is achieved for the tested configuration. Requirements and their direct evidence:

1. Runtime allocations and GPU submission through D3D callbacks: M539-M541 bring-up and M723 runtime import/allocation/ETW identities, with actual DWM-owned DMA completion.
2. GPU-fence synchronization with Present: M723 exact4274 wait/Present/signal joins and independent checkpoint count, plus completed DMA and scanout flips.
3. Small-window test followed by GPU DWM: M707 hosted client under CPU DWM, then M723 combined GPU desktop/native client with exit0 and correct composed/primary images.
4. Correct image, DWM-attributed GPU execution and instrumented no-full-frame-CPU-copy evidence: M723 matrix with the accepted bounded(a)-(d) list, M695/M697/M702 controls and final24635-map census. No whole-OS memory-writer claim is made.

The closure applies to this implementation objective. M13's distinct lifecycle, longer stability, fault/power and soak requirements remain unchanged and open as their own gates. No30-minute run was authorized or performed. After124.910s the test restored the verified CPU170 baseline; no permanent GPU deployment is claimed. Startup VSync gaps and the earlier isolated DWM049 cadence defect remain recorded, not erased by this audit.

A further independent review of DWM050 was requested; no response was present when this audit was recorded. Silence is not treated as agreement. This result changes no roadmap scope or consensus decision and does not claim such a review has completed.
