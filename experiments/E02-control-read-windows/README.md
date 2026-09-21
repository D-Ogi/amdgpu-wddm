# E02: Control read of the same registers under Windows

State: planned, blocked on E01.

## Hypothesis

A Windows kernel-mode read of BAR5 at regcalc offsets returns the same values as phase A of E01 on the same unit, for every register that is stable between two reads.

## Procedure (to be detailed in M1)

1. Install Windows on the unit next to the baseline Linux. Device shows as "Microsoft Basic Display Adapter" or Code 43; that is fine.
2. Read the E01 probe list through BAR5. Candidates for the reader, in order of preference: our own minimal read-only KMDF function driver for `1002:13FE` (no writes at all in its first version); the predecessor's IOCTL peek tool built from `ref/keshas-driver`, used strictly for reads at our offsets.
3. Compare with `evidence/linux/<E01 run>/report.json`, phase A.

## Expected

Equality on all stable registers. Any difference is either a volatile register (status, pointers) or a finding about what Windows' boot path (basic display driver, ACPI, power management) does to the GPU that Linux' does not. Both outcomes are useful; neither is a reason to write to anything.

## Result

_Not run yet._
