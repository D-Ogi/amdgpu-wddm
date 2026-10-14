# cumode - 24 or 40 compute units

Switches the BC-250 GPU between the 24 compute units it ships with and all 40. The driver (KMD 0.7.174 or
later) applies the mode when the device starts. The tool writes the setting, asks for the reboot that applies
it, confirms a healthy 40 CU start and shows what the driver did. Design, safeguards and the lab plan:
`docs/design/cu-mode.md`.

This build requires the shared state schema. Reads use the common client and preserve errors.
Only the common client supplies defaults. Setting changes hold one writer lock through backup,
write, persistence, readback and rollback. A failure reports both the original and rollback errors.
Older drivers need the matching historical tool.

```
cumode                          the window (setting, status, reboot, confirm)
cumode status                   setting, this start's mode and reason, registers per shader array, temperature
cumode set 24|40 [--disable M]  write the setting (administrator); M: WGPs kept masked, bit sa * 5 + wgp
cumode apply                    what the next start will do
cumode reboot                   restart Windows in 10 s (administrator)
cumode restart-device           restart only the display device (administrator; lab validation only)
cumode confirm [--wait S]       confirm a pending unlocked start (administrator); retries for up to S seconds
```

An unlocked start (mode 40, with or without a mask) is pending until it is confirmed. `cumode confirm` succeeds
once the desktop has run healthy for a minute, and a KMD deploy's start-health confirmation confirms it too. If the
machine restarts before that, the next start falls back to 24 and writes `CuMode = 24`; `cumode set 40` tries again.
Every line of `confirm` and the pending note of `status` name the cores the registers count, so a start of 30 cores
under a `--disable` mask never reads as 40.

40 CUs draw more power and heat: the reference measured about +30 W at 1500 MHz. The tool changes no clock and
no voltage; the lab runs at 1000 MHz / 820 mV. The temperature line needs an administrator; stop above 87 C (85 C before 2026-10-01).

Exit codes: 0 done, 1 failed, 2 bad usage or no BC-250 with this driver, 3 refused by the driver.

Build (never runs it):

```
pwsh tools\win\cumode\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\cumode
```
