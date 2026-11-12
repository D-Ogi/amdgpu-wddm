# Pending marks at an early restart of Windows (BD-369)

Unit A, Windows 11 22631, stock BIOS P5.00, 2026-11-12. The files are operator logs: the stored values of the
driver state store, the processor count that Windows reports, and lines of the KMD log ring. The lab-flavour KMDs
are builds of the branch t39/bd368-core-unlock-0x98, swapped into the installed tester.38 package.

Each arm does the same steps. A confirmed start stores the full core mask. A cold restart follows. Then Windows
restarts 5 s after the request, before the healthy milestone of that start.

## Files

| File | KMD | Arm | Result |
|---|---|---|---|
| `01-first-fix-loses-the-mask.txt` | 303195D0 | the marks are taken back in the adapter's `DxgkDdiSetPowerState` for a shutdown action | 16 logical processors, and the stored mask is 0x77 again. The log ends before its last read: the mains power failed |
| `02-probe-arm.txt` | E7D686C0 (probe) | the same KMD with a log line at each `DxgkDdiSetPowerState` and at each `\Callback\PowerState` notification, and the log ring kept to a file at both | 16 logical processors, stored mask 0x77 |
| `03-probe-kept-logs.txt` | E7D686C0 (probe) | the kept log files of arm 02 | the start that the restart ended has the notification's line at 40.070 s and no `SetPowerState` line |
| `04-notification-keeps-the-mask.txt` | 2DA9B83A | the marks are taken back at the `\Callback\PowerState` notification | 16 logical processors, stored mask 0xFF, and the new start wrote its own pending mark |

## What this shows

- At a restart of Windows 22631 on unit A, the `PO_CB_SYSTEM_STATE_LOCK` notification runs at PASSIVE_LEVEL
  with the driver's log file still writable.
- The kept log of that start has no line of the adapter's power call. Either Windows did not call it for the
  restart, or the call came when the log file was not writable any more. A registry delete at that point is not
  reliable in both cases.
- A driver that takes its pending marks back at the notification keeps the stored core mask through a restart
  before the milestone (file 04). The same steps lost the mask with the first fix (files 01 and 02).

## Limits

One board, one Windows build, one pass of arm 04. A shutdown to power-off and a sleep were not measured in this
session: the host test of `cpu.c` covers the rule for both.

Fact: M964 (docs/facts/data/kmd.yaml).
