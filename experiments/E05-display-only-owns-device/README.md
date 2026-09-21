# E05: a display-only WDDM driver owns the GPU and keeps the firmware's display

State: **run 001 done, H1-H4 hold** (2026-09-21). Evidence: `evidence/windows/2026-09-21-E05-run-001/`.

## Why

Milestone M3 is a WDDM miniport of ours that binds to `1002:13FE` and keeps the display alive by taking over
the framebuffer the firmware set up (post-display ownership), one fixed mode, no engine work. Before writing
that miniport, the cheapest way to learn whether the approach works on this board at all is to run a driver
that is known to be correct: Microsoft's KMDOD sample, **unmodified**, built from a checkout outside this
repository (`build.ps1`), with an INF that matches only our device. The sample is MS-PL licensed and is an
instrument of this experiment; none of its code enters our driver (our miniport is written against the
documented DDI, with this experiment's observations as its acceptance test).

## Hypotheses

- H1. The package installs on `PCI\VEN_1002&DEV_13FE` with a self-signed test certificate (test signing
  on, certificate in Root and TrustedPublisher) and the device starts without a problem code.
- H2. `DxgkCbAcquirePostDisplayOwnership` hands over the firmware's framebuffer on this board: the desktop
  stays visible at the firmware's mode (1920x1200 now) after the switch from BasicDisplay, no reboot needed.
- H3. The BAR5 register state is not changed by the switch (compare a `bc250rd` sweep before and after:
  a display-only driver should not touch the GPU's engines).
- H4. The driver survives disable/enable and a reboot; `pnputil /delete-driver ... /uninstall` brings
  BasicDisplay back.

## Recovery, prepared before the first install

1. Normal case, display dead but system alive: SSH still works; `rollback` below.
2. System does not come up: boot menu (8 s) has "BC-250 recovery: safe mode + network". Third-party display
   drivers are not loaded in safe mode, `sshd` is allowed there (`SafeBoot\Network\sshd`), Wi-Fi service is
   on the safe-mode list. **This path has to be proven once (one-time boot into the entry) before the
   install.** It needs a keyboard on the BC-250 to pick the entry if it is ever needed for real.
   First proof, 2026-09-21: the entry boots, but safe mode shows no network adapters (the vendor driver of
   the USB Wi-Fi dongle is not on the safe-mode list), so no SSH.
   Second proof, same day, after the owner connected the wired NIC: safe mode with networking comes up
   with the network (through the kernel debugger's virtual adapter, KDNET being on), `sshd` answers,
   `bc250rd` is not loaded (so a broken driver of ours would not be either). **Proven.**
3. Last resort: NVMe into the USB enclosure, delete the driver from the offline image.

## Procedure

```powershell
# build on the PC
pwsh experiments\E05-display-only-owns-device\build.ps1 -Sample P:\BC-250\ref\Windows-driver-samples\video\KMDOD `
     -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\e05-kmdod
# on the target, elevated (package copied to C:\BC250\e05)
pnputil /add-driver C:\BC250\e05\bc250kmdod.inf /install          # install
pnputil /enum-drivers | findstr /i /c:"bc250kmdod" /c:"oem"        # find oemNN.inf
pnputil /delete-driver oemNN.inf /uninstall /force                 # rollback
pnputil /scan-devices
```

Before and after: `bc250rd_cli sweep ... GC.` for H3, a screenshot through `bc250mon` for H2, device state
as in E02. `e05_target.ps1` does each phase on the target and logs it; `compare.py` compares the sweeps
against a noise floor taken from two sweeps with nothing changed in between.

## Result

Run 001, 2026-09-21, kernel debugger attached throughout, no bugcheck, no hang.

| | Hypothesis | Outcome |
|---|---|---|
| H1 | installs with the lab test certificate, starts without a problem code | **holds**: `pnputil /add-driver /install` bound the sample to `PCI\VEN_1002&DEV_13FE`, status OK, service `bc250kmdod` |
| H2 | post-display ownership hands over the firmware framebuffer, desktop stays at 1920x1200, no reboot | **holds**: mode unchanged in the device state, desktop and overlay present in the captures, the owner kept using the screen. A capture proves the compositor's content, not the panel; the owner is the witness for the panel |
| H3 | the switch does not change GPU register state | **holds for the GC block**: 4537 registers, 11 differ from the control sweep, all 11 inside the noise set of 36 that differ between two sweeps with nothing changed (`comparison.txt`) |
| H4 | survives disable/enable and a reboot; uninstall brings Basic Display back | **holds**, all three. After the rollback Windows picked 200 % scaling for the "new" Basic Display instance: cosmetic, noted |

What this settles: a display-only WDDM driver can own this device and live on the firmware's framebuffer, so
ADR 0006 stands and our own miniport (`driver/kmd`) has an acceptance test it can be held against, step for step.

Found on the way, more important than the experiment itself: **our register sweep is not side-effect free.**
The noise set is mostly damage done by the first sweep: every `GCVM_INVALIDATE_ENGn_SEM` reads 1 the first time
and 0 the second (a read *acquires* the semaphore and nothing releases it), the `CP_*_HEADER_DUMP` registers
advance on every read, and `GRBM_READ_ERROR` latches an error for the address of `GRBM_GFX_CNTL`. Consequences
are in `docs/facts.md` (M25, and the correction of M24).
