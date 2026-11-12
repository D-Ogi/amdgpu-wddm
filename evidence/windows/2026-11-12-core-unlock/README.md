# Eight CPU cores from Windows on the stock BIOS, lab arms (BD-368, BD-369)

Unit A, Windows 11 22631, stock BIOS P5.00, 2026-11-12 04:33Z to 07:43Z. The files are the operator logs of each
arm: the output of `bc250kmd_cli cpu`, the processor count that Windows reports, the `cpu:` and `smu metrics:`
lines of the KMD log ring, and the watts of the wall plug. One lab network address in file 06 is redacted. The
candidate KMDs are lab and release builds of the branches t39/bd368-cpu-confirm and t39/bd368-core-unlock-0x98,
swapped into the installed tester.38 package.

## Files

| File | KMD | Arm | Result |
|---|---|---|---|
| `01-0x2c-release-kmd.txt` | 5C97A127 (tester.38) | `cpu cores 8` sends queue 0 message 0x2C with 0xFF, then a warm restart | 12 logical processors, and the stored mask went back to 0x77 |
| `02-guard-and-0x2c-candidate.txt` | D71CF9B2 | the same message with the guard fix, two warm restarts | accepted with status 0 each time, 12 logical processors each time |
| `03-0x98-first-unlock.txt` | 5A94C807 | queue 3 message 0x98 with the argument SMN 0x0115A870, then a warm restart | 16 logical processors, 8 cores. A cold boot before the send leaves 12 |
| `04-early-shutdown-drops-mask.txt` | 707E9DC4 | the device is enabled and Windows shuts down one second later | the next two starts run the stock mask (BD-369) |
| `05-metrics-table-raw-8-cores.txt` | 707E9DC4 | second unlock, then two raw reads of the SMU metrics page | 16 logical processors. Both reads give the same 61 words |
| `06-6-core-control-telemetry.txt` | 6A2B18AA | telemetry with 6 cores on the new parser. The unlock arm of this file is not valid: a shutdown ran during the enable | power 36 W to 37 W at 88 W to 92 W on the plug |
| `07-8-core-telemetry-and-way-back.txt` | 6A2B18AA | third unlock, telemetry with 8 cores, then queue 0 message 0x2C with 0x77 and a warm restart | 79 tables read, power 36.7 W to 40.5 W at 87 W to 93 W on the plug. After 0x2C with 0x77: still 16 logical processors |
| `08-power-off-gives-6-cores.txt` | 6A2B18AA | stored mask 0x77, shutdown, AC off and on | 12 logical processors, mask 0x77 in force |

## What this shows

- Queue 3 message 0x98 with the address of the core mask register and one warm restart gave 8 cores three times
  (files 03, 05, 07). The firmware of the stock BIOS is not changed.
- Queue 0 message 0x2C is accepted with 0xFF and with 0x77, and the processor count did not change in either
  direction (files 01, 02, 07).
- A cold boot gives the stock mask back (files 03 and 08). The way back to 6 cores is a power-off.
- With 8 cores the stock SMU still writes a 244-byte metrics table. Word 17 is 0x181F0025: the low half
  holds 37, where the 6-core table holds the graphics clock in MHz, and the high half holds 6175, which is the
  graphics temperature of 61.75 C in hundredths of a degree (file 05). The temperature, the currents and the power values after it read as plausible values.
- A parser that takes such a table without its clock reads the power of the 8-core state (file 07) in the same
  band as the 6-core control (file 06).
- The log lines "mask 0x77 in force ... 16 processors" in file 07 are a defect of that candidate: it reported the
  last accepted message, not the processor count. A later commit of the branch reports the mask from the count.

## Limits

One board. The 60 s load on 16 threads in the session did not check results per core. The shutdown from the
8-core state took 148 s and more than 150 s in two arms and 37 s in a third. The cause is not known.

Facts: M961 and M962 (docs/facts/data/hardware.yaml), M963 (docs/facts/data/kmd.yaml).
