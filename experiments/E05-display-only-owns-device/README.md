# E05: a display-only WDDM driver owns the GPU and keeps the firmware's display

State: prepared, not run. Blocked on the recovery path: its first proof failed (see Recovery, point 2).

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
   the USB Wi-Fi dongle is not on the safe-mode list), so no SSH. Not proven until that is fixed and re-run.
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
as in E02.

## Result

_Not run yet._
