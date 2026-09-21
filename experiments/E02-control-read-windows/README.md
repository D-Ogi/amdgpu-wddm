# E02: Control read of the same registers under Windows

State: prepared, not run. Windows is on the NVMe (`tools/wininstall`), the reader is built (`tools/win/bc250rd`).

## Hypotheses

- H1. A Windows kernel-mode read of BAR5 at regcalc offsets returns the same values as the Linux
  pre-driver sweep of E03 on the same unit, for every register that is stable between two reads.
- H2. With only the Microsoft Basic Display driver on the device, memory decoding is enabled and BAR5 is
  assigned (the same physical address as under Linux, `0xFE900000`, is likely but not required).
- H3. The registers that prior art reported as "locked" or "zero" under Windows read the same non-zero
  values as under Linux. If so, their findings were addressing errors under Windows too, not a Windows
  or firmware lock.

## Procedure

1. Boot unit A from the NVMe. Unknown until tried: whether the firmware boots this disk. The first boot
   runs `specialize` and the first-logon script; afterwards the machine should answer on SSH over Wi-Fi.
2. Record the platform state: `bcdedit` (test signing), Device Manager status of `PCI\VEN_1002&DEV_13FE`
   (expected: Basic Display Adapter), resources of the device, `sfc /verifyonly`.
3. Load `bc250rd.sys`, run `bc250rd_cli info`, then `sweep reglist.txt GC.` through SSH with the log on
   the PC side. Then the other blocks, one at a time: `HDP.`, `NBIO.`, `OSSSYS.`, `MP0.`, `MP1.`, `MMHUB.`.
4. Compare with `evidence/linux/2026-09-21-E03-init-trace/sweep-before-*.log` line by line.

## Safety

Read-only by construction (read-only mapping, no write IOCTL, allow-list of offsets that were harmless
under Linux). The state of the GPU under Windows may differ (power gating by the firmware or the basic
display path), so a hang is possible even on these offsets: that is why the sweep is streamed and run one
block at a time. Each hang costs the owner a power cycle.

## Expected

Equality on all stable registers. Any difference is either a volatile register (status, pointers) or a
finding about what Windows' boot path does to the GPU that Linux' does not. Both outcomes are useful;
neither is a reason to write to anything.

## Result

_Not run yet._
