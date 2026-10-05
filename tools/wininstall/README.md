# wininstall - Windows on the BC-250's NVMe, prepared from another PC

The BC-250 has no easy way to run Windows Setup (no TPM, keyboard-less bench, Wi-Fi only on our unit).
Instead the NVMe disk is attached to a PC over USB, an image is applied to it with DISM, and the disk is
then moved into the board. The first boot configures itself from an answer file.

| File | Purpose |
|---|---|
| `make_payload.ps1` | Renders the answer file and the Wi-Fi profile from the secrets folder, collects the SSH public key and the OpenSSH zip. Output lives in the secrets folder, never in the repository |
| `unattend.template.xml` | Answer file: `specialize` (computer name, no device encryption, no drivers from Windows Update) and `oobeSystem` (no OOBE questions, local admin account, auto logon, first-logon script) |
| `firstlogon.ps1` | First logon: Wi-Fi profile, OpenSSH server with key login, RDP, ping, power settings, kernel dumps without auto reboot |
| `install_to_disk.ps1` | Partitions the disk, applies the image, writes boot files and a BCD with test signing on, injects drivers, copies the payload. Dry run unless `-Execute` |

## Use

```powershell
pwsh tools\wininstall\make_payload.ps1 -SecretsDir $env:BC250_ROOT\secrets -OpenSshZip <OpenSSH-Win64.zip>

# elevated; first without -Execute, read the plan, then with it
pwsh tools\wininstall\install_to_disk.ps1 -DiskNumber 7 -ExpectedDiskName 'WD Blue SN570*' -DataLetter H `
    -ImageFile <install.wim|install.esd> -PayloadDir $env:BC250_ROOT\secrets\win\payload `
    -WorkDir $env:BC250_ROOT\scratch\win-install -DriverDir <extra drivers> `
    -WipeDisk -ExpectedDiskSizeGB 1863 -WindowsSizeGB 500 [-Execute]
```

`BC250_ROOT` is the workspace root, by default the parent directory of this repository.

Without `-WipeDisk` the script shrinks the named data partition and installs next to it, touching nothing
else except the disk's EFI system partition (which it backs up first).

## Safety

- `-WipeDisk` refuses unless the disk number, the friendly name, the size, the bus type (USB only), the
  expected data drive letter and "not this PC's boot or system disk" all agree, and the disk carries no
  other lettered volume. The dry run lists every partition that would go.
- `bcdboot` is called with `/s` on the target disk's own EFI partition. Measured: in that form it does not
  touch the firmware boot menu of the PC it runs on (it even says that `/addlast` is ignored "when custom
  volume is specified"); the script still compares the firmware entries before and after. It does write
  the fallback loader `EFI\Boot\bootx64.efi`, which is what a board that has never seen the disk boots.
- Applying an image with DISM runs nothing from the installation media: no Setup, no `$OEM$` scripts.
  The system starts unactivated, which is enough for driver work.

## Notes

- OpenSSH server is not part of the image and cannot be added as a Windows capability without a network,
  so the Win32-OpenSSH release zip from Microsoft's GitHub project is installed instead.
- Drivers needed for first contact must be injected offline. For unit A that is the ASUS USB-AC58 Wi-Fi
  adapter (`USB\VID_0B05&PID_19AA`), which neither the image nor Realtek's generic package covers; the
  vendor's Windows 10 x64 package does. Driver binaries are not kept in this repository.
- Test signing is switched on in the offline BCD, so the first boot already accepts test-signed drivers.
  Secure Boot is off on unit A (facts M14).
