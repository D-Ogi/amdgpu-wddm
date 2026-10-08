# Trial 469: Witcher 3 HIGH + all RT effects, W3 RT effect-cost series arm A (first of two)

468 did not start: the owner's display mode changes moved the KMD epoch (31 -> 97) between run-m157's confirm pre-step and the Capture witness (flags 7, CONFIRMED missing). Heartbeat and an orphan sampler removed, start confirmed again (epoch 97, flags 15).

Series (2026-10-08, scratch/w3-rt-cost, branch measure/w3-rt-effect-cost): `bash scratch/w3-rt-cost/run-arm.sh N PRESET [PERFTEST]`
= SESSION_ADAPTER=adapter132 SESSION_ACCEPTED=1 SESSION_SAMPLER=1 SESSION_EXPERIMENT=none SESSION_VSYNC=false
SESSION_LIMIT_FPS=240 SESSION_ETW_ARGS="-Seconds 40 -LatestB 447 -FpsSeconds 90" run-m157.sh N PRESET 600 (direct start),
plus drive-still.sh N, the intro skip and plug sampling; overlay summary poll paused (graphics-summary.pause) for the session.
Lab: hand deviation KMD 0.7.216.20 (sys 7580A8F7), router 93F707BB, D3D12 shell BBB5803E (= adapter132), engine 348117F1,
ICD 822134D0, zink b26, hosted ICD CD360941; lab-baseline.json re-pinned to it (handdev-kmd20-baseline.py). 40 CU.
Arms (presets game-recon/presets/w3rt-*.txt = HIGH + only the [Rendering/RT] lines; RT GI is on whenever EnableRT is):
A w3rt-all (GI perf, reflections, shadows perf, AO), B w3rt-gi, C w3rt-refl (GI + reflections), D w3rt-shadow (GI +
shadows), E w3rt-ao (GI + AO), F w3rt-all + RADV_PERFTEST=rtwave64, G w3rt-all + RADV_PERFTEST=cswave32 (no BVH build
quality knob exists in ICD 31844893 for gfx10), H w3rt-off (RT off control), A again at the end (spread and drift).
- Question: where does the HIGH + RT frame time go, per RT effect and under the RADV RT knobs?
- Conjecture C74: every arm reaches the Kaer Morhen room and holds window B with no GPU fault, TDR, removed device or
  runner thermal stop; the all-effects rate is under the 12.0/s of 462 (462 had no RT AO).
- Kill: any of those faults, or no world within the bound.
- Safety: Tctl >= 87 C held 10 s or >= 89 C (runner stop), plug watts sampled, bound 600 s. RT stays ON afterwards:
  the lab's dx12user.settings is restored from this session's pre-session backup after the series.
