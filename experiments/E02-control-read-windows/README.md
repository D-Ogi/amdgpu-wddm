# E02: Control read of the same registers under Windows

State: run 001 done on 2026-09-21. Evidence: `evidence/windows/2026-09-21-E02-run-001/`.

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

## Result (run 001)

- H1 holds. 5542 registers read through `bc250rd.sys`, no hang, 5074 bit-identical to the Linux
  pre-driver reference, including every register our earlier conclusions rest on. The 468 others are
  explained without Windows: 457 differ in 1-5 bits of uninitialized-looking state, the rest are
  clocks and three PSP mailbox registers carrying state from the previous Linux session (facts M19;
  the cause is a hypothesis until a sweep after a recorded cold start confirms it).
- H2 holds, with one surprise: decoding is on and BAR5 is assigned, but at `0xFE800000`, not where Linux
  put it (facts M20).
- H3 holds. The "locked" and "zero" findings of prior art are not a Windows or firmware effect: read at
  the correct offsets, the registers show the same values under Windows as under Linux.
- Not tested: writes. `bc250rd` cannot write. Whether `SCRATCH_REG0` and `GRBM_GFX_INDEX` accept writes
  under Windows as they did under Linux (M3) is the obvious next control, and needs a deliberate,
  separately reviewed write path limited to those two registers.
- A self-signed test certificate is enough to load a kernel driver on this install with test signing on.
