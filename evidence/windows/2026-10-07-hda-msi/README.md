# DP audio rate and the interrupt mode of the HD Audio function on unit A (2026-10-07, BD-092)

Unit A, Windows 11, desktop on the GPU route. The operator gives the kernel driver as 0.7.216.3 and the boot as
2026-10-07T05:15:35Z. No file of this directory records these two values. Three bounded runs over SSH
(`python tools/win/target.py ps <script>`), 05:17Z to 05:21Z:

1. `hda-inventory.ps1` (read only) -> `e1-inventory.txt`.
2. `run-probe.ps1 -Label base -Secs 5` -> `e2-base.txt`: seven WASAPI streams on the default render endpoint.
3. `hda-msi.ps1 -On` -> `e3-msi-on.txt`, then `run-probe.ps1` again -> `e3-msi-probe.txt`.

`wasapi-probe.cs` is the probe (built on the lab with the .NET Framework `csc.exe`). It plays a 1 kHz tone and
compares the device position (`IAudioClock`) with the elapsed time. `device_rate_all` is position / elapsed over
the whole run, `device_rate_after2s` the same after the first 2 s. `run-probe.ps1` kills a probe after 25 s and
keeps only the samples at about 0.25 s steps in the output file.

## Files

| File | Content |
|---|---|
| `e1-inventory.txt` | PnP, registry and IRQ facts of the HD Audio function 1002:13FF and of the GPU 1002:13FE |
| `e2-base.txt` | the probe with the inbox setting (line-based interrupt) |
| `e3-msi-on.txt` | `MSISupported` 0 -> 1 on the 13FF instance and `pnputil /restart-device` of that device only |
| `e3-msi-probe.txt` | the probe after that change (the script printed result lines only, no samples) |
| `scripts/` | `hda-inventory.ps1`, `hda-msi.ps1`, `run-probe.ps1`, `wasapi-probe.cs` |

## What it shows

1. **The HD Audio function has a line-based interrupt.** `e1-inventory.txt`: service `HDAudBus`,
   `MSISupported=0`, `DEVPKEY_PciDevice_InterruptSupport` 3, `InterruptMessageMaximum` 1, IRQ 53, shared with 0
   other devices. The inbox `hdaudbus.inf` (`DriverVer` 05/30/2025, 10.0.22621.5415) writes `MSISupported` 0.
   `hdaudbus.sys` is 10.0.22621.5415. The GPU function has `MSISupported=1` and a message interrupt (IRQ
   4294967277).
2. **Before the change (`e2-base.txt`):**

   | Mode | Rate | `init_ms` | Result |
   |---|---|---|---|
   | exclusive poll | 48000 | 9614.2 | `device_rate_all` 1.0000, 0 timeouts |
   | exclusive event | 48000 | 7094.7 | killed after 25 s, position 0.0425 s after 6.035 s |
   | exclusive poll | 44100 | 9627.0 | `device_rate_all` 1.0000 |
   | exclusive event | 44100 | 7089.5 | `device_rate_all` 0.0051, events 1, timeouts 3, `max_gap_ms` 2009.7 |
   | shared event | 44100 float | 8130.5 | `device_rate_all` 0.6549, events 324, `mean_gap_ms` 15.47 |
   | shared poll | 44100 float | 8125.1 | `device_rate_all` 0.6480 |

   `start_ms` is 3523.2 to 3589.9 in every mode.
3. **The change (`e3-msi-on.txt`).** Before: `status=OK MSISupported=0 irq=53,53`. `pnputil`: "Device restarted
   successfully." After: `status=OK MSISupported=1 irq=` (the WMI IRQ resource list is empty).
4. **After the change (`e3-msi-probe.txt`):**

   | Mode | Rate | `init_ms` | Result |
   |---|---|---|---|
   | exclusive poll | 48000 | 9.4 | `device_rate_all` 1.0000 |
   | exclusive event | 48000 | 4.4 | 1.0000, events 470, timeouts 0, `mean_gap_ms` 10.64 |
   | exclusive poll | 44100 | 8.5 | 1.0000 |
   | exclusive event | 44100 | 9.6 | 1.0000, events 494, timeouts 0, `mean_gap_ms` 10.13 |
   | shared event | 44100 float | 19.0 | `device_rate_all` 0.9983, `device_rate_after2s` 1.0000, events 494 |
   | shared poll | 44100 float | 17.1 | `device_rate_all` 0.9999, `device_rate_after2s` 1.0000 |

These files do not show whether audio was audible after the change. They also do not show whether the lab keeps
`MSISupported=1` after this run (`hda-msi.ps1 -Off` restores the old value). The `-samples.txt` files on the lab
are not copied.

Privacy changes: see `REDACTED.txt`.
