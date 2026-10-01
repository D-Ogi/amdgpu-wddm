# Trial 254: M15.7 The Witcher 3 HIGH with RT started through the Steam client, registered triplet (adapter109), 7-min session

`SESSION_LAUNCH=steam run-m157.sh 254 high-rt`: the registered triplet since 252 (shell adapter109 0CE9D4F8 =
bc250-win daa465f5, the application profile; engine 15E3E24E, ICD 2A13235D; BC250_TRIAL_ACCEPTED=1, nothing swapped),
KMD 0.7.193.1 with DPM on, ceiling 2000 MHz, GPU DWM, preset HIGH with RT on, AAMode 0/1, FG off, DRS off, native
1080p, LimitFPS 60. New: BC250_TRIAL_LAUNCH=steam (run.py, game-runtime.ps1 since 254): the trial asks the running
Steam client to start app 292030 (`steam.exe -applaunch 292030`); Steam's only launch entry runs redprelauncher.exe,
whose launcher-configuration.json names one executable, bin\x64_dx12\witcher3.exe. The game inherits Steam's
environment: no AMDGPU_WDDM_D3D12_EXPERIMENT, no debugger, no shader dump. The switches of 246/249/250 come from the
application profile HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\witcher3.exe, Experiment =
present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff (set 14:20Z by
scratch\m15\app-profile\set-profile.ps1; the result records the value it found). Interactive: the Kaer Morhen
interior route of 245/246/249/250.

- Question: does the start through the Steam client reach the same rendering as 250 (the M15.7 gap "the start through
  the Steam client recorded"), with ray tracing available to the game only through the driver's application profile?
- Expected: Steam starts REDprelauncher, which starts witcher3.exe within a few seconds and exits; the game's modules
  are our triplet; RT on (reflections and RT lighting as in 250's shots), GPU 3D near 98 %, about 9.3/s in window B;
  functional-restored, no fault, no TDR.
- Refutation: (a) no witcher3.exe within 90 s (a dialog of Steam or the prelauncher: the launch screenshots show
  which), (b) the game starts but offers no RT (RT effects absent, GPU busy far below 98 %: the profile not read, or
  read as Invalid), (c) a fault, hang or 0x116.
- Thermal: KMD cap drops at 87 C, floor at 90 C; HTTP guard ends the game after two readings above 87 C.

## Result
2026-10-01 14:20-14:27Z: **no game start; operator-closed-in-place** (nothing swapped, Verify rerun exit 0 at 14:3xZ, task removed). The trial asked Steam for app 292030 at 14:22:38Z with the profile recorded (app_profile = the five switches, experiment none), but Steam had an update of The Witcher 3 pending since 13:12Z (depots 292031..370009, 550 MB download, 45.6 GB staged) and `-applaunch` started that update instead (Steam content_log: "Update Queued, Update Running" 14:22:38Z); runtime: "Game did not start through Steam within 90 s"; the supervisor's Verify then ran out of its 25 s budget on the saturated disk (recovery-unverified, closed by close-inplace.ps1 -AfterVerifyTimeout). The update finished 14:25:59Z (BuildID 25646871, witcher3.exe 9406ECCC = 5.0.0.1044392; every earlier session ran C272B2C2 = 5.0.0.1041720) and Steam then ran the queued launch outside the trial: REDprelauncher (updated the same day, a Qt stub) started the REDlauncher 5.5.0.5 installer (msiexec 14:26:08Z) instead of the game and was ended by the operator 14:27Z; no witcher3.exe ran. Lessons, in game-runtime.ps1 for 255: launch only when appmanifest StateFlags is 4 (no pending update), and pass `--launcher-skip` after the app id (a REDprelauncher option: "Skipping REDlauncher due to commandline options"), which Steam appends to its launch entry as the game's launch options would. Steam's launch entry 0 of 292030 (appinfo v29 record, app-profile\parse-appinfo.py): executable redprelauncher.exe, no arguments.
