# Trial 559: The Witcher 3 Remaster, HIGH with ray tracing, interactive, EnableRingVmFlush gate 0

Package 0.7.216.100-tester.28 (KMD build 0.7.216.32, ABI 0x000700D8), installed clean slate by its own installer on 2026-10-11 (boot 18:25:59Z), D3D12 triplet promoted in native-caps557/558. Nothing is swapped for this trial: the files under test are the installed ones (shell adapter138 DDA920E7, engine 80217882, ICD F43E4392).

- Question: the gate-0 half of the EnableRingVmFlush game arm (bc250-win docs/design/gfx-submit-root-serialization.md section 3, "Open: the game frame time"). `EnableRingVmFlush` is absent: the VM flush is the MMIO write before the frame, engine 17.
- Reference: 12.9 present/s from b28 native-caps540 (tester.26), 9.66 per GHz; b27 530 12.8/s, 9.50 per GHz; b26 519 12.5/s, 9.35 per GHz. Trial 560 is the gate-1 half on the same package, save, scene and window.
- Method: `bash scratch/ringflush-game/run-w3.sh 559` (run-m157.sh 559 high-rt 1200 with the w3-high-rt arm's environment and the tester.28 engine and ICD). The operator drives the game from half-scale shots, presses CONTINUE, does not move Geralt after the load, waits 30 s in the world, sends `world` once, and sends no command for the 120 s of window B. No upscaler, no dynamic resolution, VSync off, limit 240.
- Read after the session: `ringflush.ps1 -Step read` (GfxStart line, `ring VM flush off: N frames`, faults, fence timeouts, ResetEngine, Display 4101), health, plug watts.
