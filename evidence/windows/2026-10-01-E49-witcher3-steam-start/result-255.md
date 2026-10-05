# Trial 255: M15.7 The Witcher 3 HIGH with RT started through the Steam client (second try, game build 25646871)

`SESSION_LAUNCH=steam run-m157.sh 255 high-rt`: plan-254 with its two lessons in game-runtime.ps1: the launch is
refused unless appmanifest_292030.acf reports StateFlags 4 (no pending update), and Steam receives
`-applaunch 292030 --launcher-skip` (REDprelauncher's own option: it skips REDlauncher and starts the game). Registered
triplet since 252 (shell adapter109 0CE9D4F8, engine 15E3E24E, ICD 2A13235D; BC250_TRIAL_ACCEPTED=1, nothing
swapped), KMD 0.7.193.1, DPM on with the 2000 MHz ceiling, GPU DWM, preset HIGH with RT, AAMode 0/1, FG off, DRS off,
native 1080p, LimitFPS 60. The game is the build Steam installed at 14:25:59Z (BuildID 25646871, witcher3.exe
9406ECCC = 5.0.0.1044392): **the first session on this build**, so a failure separates as follows: no witcher3.exe =
the Steam/launcher path; a menu or loading failure = the new build. RT comes only from the application profile
HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\witcher3.exe (the five switches of 246/249/250).

- Question: the M15.7 gap "the start through the Steam client recorded", with RT available to the game through the
  driver's application profile only; and whether build 1044392 renders as 1041720 did (250).
- Expected: REDprelauncher logs a skip and starts bin\x64_dx12\witcher3.exe within ~10 s; modules = our triplet; RT on
  (reflections and RT lighting as in 250's shots); 3D near 98 %, about 9.3/s in window B; functional-restored.
- Refutation: (a) no witcher3.exe within 90 s; (b) the game starts without RT (no RT effects, GPU busy far below
  98 %); (c) a fault, hang or 0x116; (d) a rendering difference against 250's route shots.
- Thermal: KMD cap drops at 87 C, floor at 90 C; HTTP guard ends the game after two readings above 87 C.

## Result
2026-10-01 14:31-14:41Z, registered triplet (adapter109 0CE9D4F8), KMD 0.7.193.1, DPM 2000, GPU DWM, game build
25646871: **functional-restored** (operator quit at 303 s of game time, elapsed 337.9 s, tree closed, baseline and
settings restored, job empty, no survivors). **Started through the Steam client**: the runtime recorded app_profile =
the five switches and experiment none, StateFlags 4 build 25646871, `steam.exe -applaunch 292030 --launcher-skip` at
14:32:59Z, REDprelauncher after 2 s, witcher3.exe (9406ECCC, 5.0.0.1044392) after 4 s in session 1, no REDlauncher,
no installer; our three modules at 41 s from C:\BC250\m15\registration002 (0CE9D4F8, 15E3E24E, 2A13235D), window at
41 s, menu at 87 s, world after E + 20 s. RT came from the application profile alone (the trial set no switch) and
was on: RT reflections on the floor in the starting room (shot-001), valley through the balcony door (shot-004),
shots 001 and 003-011 in scratch\m15\control\native-caps255, correct image (the light bands at Geralt's knees in
005/008 are the boot cuffs, also in 250's shot-003). No fault, no TDR: KMD "no TDR", 0 timeouts, 0 refused, 9940
hardware flips; spirv-fail empty.
**Window B (t=176 s, 40 s, walking): game 9.1/s, median 109.2 ms, p95 142.6, p99 197.6, max 231.6 ms, 0 > 250 ms**
(250: 9.3/s, median 107.0); DWM followed at 9.1/s. GPU counters (our LUID): **window B 3D mean 98.3 %, min 90.2 %**;
dedicated max 984 MB, shared 550 MB (250 read the same way: 962 / 554; its "4096 MB" was another column); machine CPU
48 % in window B, DWM 2.8 % of one CPU. DPM: 2000 MHz at all 15 clock readings of window B (every 2.5 s; 66
of 119 in the session) and at the six HTTP guard readings 14:35:15-14:37:52Z (busy 57-100 %), Tctl peak 86.8 C at
14:37:38Z, below the 87 C guard. Game peak working set 3938 MB, 59 threads.

Reading: M15.7's "start through the Steam client" is recorded: Steam -> REDprelauncher -> witcher3.exe on our
registered driver, RT available to the game through the driver's application profile only, same frame rate and GPU
load as the direct start of 250 (-2 %, within spread), on the new game build. HIGH+RT stays GPU-bound (K95): the new
build moved nothing visible there. Tctl 86.8 C at 2000 MHz is 0.2 C below the guard: the next HIGH+RT session may
hit the cap step (by design, not a fault).
