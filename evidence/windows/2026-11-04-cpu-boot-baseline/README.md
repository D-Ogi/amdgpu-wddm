# CPU boot baseline and start read retry, lab arm (BD-295, BD-297)

Unit A, Windows 11 22631, 2026-11-04 17:01Z to 17:49Z. Branch t38/bd295-cpu-boot-baseline, lab-flavour KMDs
swapped into the installed tester.37 package (swap-sys.ps1, test signer AFCA4B1A...). The surface ran with
CpuTune 1. `arm-log.txt` is the operator log: the output of `bc250kmd_cli cpu` and the `cpu:` lines of the KMD log
ring after each step. The PnP instance path and the state hash are redacted.

## Steps

| UTC | KMD | Step | Result |
|---|---|---|---|
| 17:02 | 02CF4015 | boot | queue 3 message 0x36 refused with 0x80000011 at 3.584 s. The read stage ran once and the start sent nothing (BD-297) |
| 17:03 | 02CF4015 | `cpu readback` | queue 3 answered, applied cap 0 C, firmware cap 100 C: the readback applied nothing |
| 17:36 | 6C948F90 | boot, LabCpuRevertFault 3 | the read stage answered at attempt 1, the default cap of 87 C went in |
| 17:36 | 6C948F90 | `cpu set clock 3000 uv 2`, device cycle | bit 1 refused the stop revert. The next start logged the leftover (what 0x1, 3000 MHz, 2 steps) and adopted the boot record (3500 MHz, cap 100 C). Bit 2 refused the first restore. Attempt 2 sent 3 of 3 steps: 3500 MHz, 0 steps, 87 C |
| 17:37 | 6C948F90 | selector deleted, set, `cpu keep`, device cycle | the start adopted the boot record and sent the stored 3000 MHz, 2 steps, 87 C |
| 17:38 | 6C948F90 | `cpu reset` | 3500 MHz, 0 steps, 87 C sent |
| 17:48 | 8F955FB4 | LabCpuRevertFault 4, device cycle | bit 4 refused the first start read stage. Attempt 2, 3.2 s later, answered and the default cap of 87 C went in |

## What this shows

- The SMU can refuse the first read stage of a start (row 1). The refusal did not come back in the later boot
  and the two device cycles without bit 4, so its cause is not known.
- With the fix, the worker tries the read stage again and the start settings follow it (last row).
- An earlier start's leftover goes back to the boot baseline, not to the leftover itself (rows 4 and 5).

Fact: M960 (docs/facts/data/kmd.yaml).
