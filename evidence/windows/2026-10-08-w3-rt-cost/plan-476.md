# Trial 476: Witcher 3 HIGH, W3 RT effect-cost series arm G all RT + cswave32: preset w3rt-all with RADV_PERFTEST=cswave32 (marker C:\BC250\tools\radv-perftest.txt, cleared after the session)

`ARM_LABEL="G all RT + cswave32" bash scratch/w3-rt-cost/run-arm.sh 476 w3rt-all cswave32` - method, lab state, conjecture C74, kill and
safety rules as plan-469.md (same location, drive-still input, window B 90 s GPU only, 1920x1080 exclusive fullscreen,
FXAA, no upscaler/FG/DRS, VSync off, LimitFPS 240, direct start, no experiment, overlay summary poll paused).
Expected closure: recovery-unverified (Verify "Confirmed CPU baseline required": the game's 1920x1080 mode commit moves
the KMD epoch, as 466/469); the runner re-confirms the start and checks the stack identities by hand.
Knob launch after the in-game sweep 474 (coordinator, 2026-10-08): load, the drive-still spot, one window B, quit; no menu work. Compared per GHz with 474's in-session all-RT control A-ctl and with 469. RADV_PERFTEST=cswave32 (compute and inline ray queries in wave32) stands in for the missing BVH build-quality knob on gfx10; measurement arm only.

## Result
2026-10-08: marker written at 01:24:50Z after "package flushed" (run-arm.sh), preflight admitted it, the game ran with
radv_perftest cswave32 (game log). Start Running 01:24:48Z, the runner's intro skip sent Space x3 at 01:25:54Z, and
the owner pressed Space by hand at about 01:28Z while the game sat at the intro (development PC clock). World 01:28:11Z,
window B 01:28:52-01:30:23Z lab clock (90.2 s). Game pid 5280: 12.6/s, median 77.42 ms, p95 90.80, p99 96.30,
1416 MHz, Tctl 77.8-85.0 C, 8.90 per GHz (474 A-ctl 8.80, 469 8.79); at 1.5 GHz with elasticity 0.75 13.2/s, the same
as A-ctl. Shots correct (RT reflections on the floor). Closure as expected: recovery-unverified (Postflight), start
re-confirmed (flags 15, epoch 157), identities unchanged, marker absent. Lab W3 settings restored from the pre-series
backup dx12user.settings.20261007T232002Z afterwards (RT on, AO off, 1280x720 windowed).
