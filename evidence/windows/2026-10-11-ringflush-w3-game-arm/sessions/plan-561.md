# Trial 561: The Witcher 3 Remaster, HIGH with ray tracing, interactive, EnableRingVmFlush gate 1

Package 0.7.216.100-tester.28 (KMD build 0.7.216.32, ABI 0x000700D8), installed clean slate by its own installer on 2026-10-11, D3D12 triplet promoted in native-caps557/558. Nothing is swapped for this trial: the files under test are the installed ones (shell adapter138 DDA920E7, engine 80217882, ICD F43E4392). The only change against trial 559 is `EnableRingVmFlush` = 1 and the restart that makes GfxStart read it.

- Question: the gate-1 half of the EnableRingVmFlush game arm (bc250-win docs/design/gfx-submit-root-serialization.md section 3, "Open: the game frame time"): the VM flush is 29 dwords on the gfx ring in front of every frame, engine 1, and no MMIO flush runs.
- Reference: trial 559 (gate 0, same package, save, scene and window); b28 540 12.9/s, 9.66 per GHz.
- Method: `bash scratch/ringflush-game/run-w3.sh 561`, exactly as 559: CONTINUE, no movement after the load, 30 s in the world, `world` once, no command for the 120 s of window B.
- Stop rule of the design (section 3, step 5): a `wddm: timeout` line, a 0x116 or a black screen ends the arm; then EnableRingVmFlush is removed and Windows restarted, the log ring and the journal kept.
- Read after the session: `ringflush.ps1 -Step read` (GfxStart line `gfx: VM flush on the ring (EnableRingVmFlush), engine 1`, `ring VM flush on: N frames`, faults, fence timeouts, ResetEngine, Display 4101), health, plug watts.
- Repeat of 560: trial 560 was refused at Capture ("Competing test task") before any mutation and before the game started. The trial's own ETW task BC250-Etw560 was already running when the late Capture phase (two minutes after a boot) listed the running tasks. etw-start.ps1 now waits for the trial's Capture-done.json before it registers the ETW task.
