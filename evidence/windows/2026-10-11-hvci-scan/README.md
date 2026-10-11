# Memory integrity (HVCI) readiness scan on unit A (2026-10-11)

Unit A, Windows 11 Pro, package 0.7.216.100-tester.27 installed (kernel driver 0.7.216.28, `bc250kmd.sys`
SHA-256 `8A873F32`, state Running), test signing on. One read-only run over SSH at 2026-10-11T08:10:19Z
(`python tools/win/target.py ps scripts/lab-hvciscan.ps1`). The script changes nothing on the machine.

The tool is the Microsoft memory integrity scan tool `hvciscan_amd64.exe` from the Microsoft Download Center
(item 105217). SHA-256 `CD1BBE921CAF4CF3283D181B1CB5FEE06FD02A67772A45F86A78CBB6AAA40E4E`, Authenticode
status Valid, signer Microsoft Corporation (both lines are in `run-001.txt`).

## Files

| File | Content |
|---|---|
| `run-001.txt` | the `Win32_DeviceGuard` values, the path, state and hash of our kernel driver, and the tool's output |
| `scripts/lab-hvciscan.ps1` | the script that wrote `run-001.txt` |

## What it shows

1. **The driver scan passes.** The tool prints `HVCI incompatible driver scan passed!` and exits with 0, with
   `bc250kmd.sys` 0.7.216.28 loaded.
2. **VBS cannot start on this platform.** `VbsGetIssues` is `0x0000048C` and the tool names four missing
   requirements: Secure Boot capability, IOMMU, the ACPI WSMT table and hardware virtualization.
   `VbsIsRecommended` is 0. `Win32_DeviceGuard` gives `VirtualizationBasedSecurityStatus` 0 and no configured
   or running security service. `AvailableSecurityProperties` is `4,5,7,8`.

## What it does not show

- It is not a run with memory integrity on. VBS does not start on unit A, so no driver loaded under HVCI.
- It says nothing about the driver signature. The driver is test-signed.
- The output does not list which drivers the tool scanned or which checks it applied. These are the tool's.

No identifier needed redaction: the files hold no address, serial, MAC or SSID.
