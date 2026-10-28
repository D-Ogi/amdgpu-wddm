# tdrclient - a controlled TDR for the TDR recovery trials

`bc250tdr.exe` asks dxgkrnl for a TDR on the BC-250 adapter (PCI `1002:13FE`), without a GPU hang. It is the
stage S0 tool of `docs/design/tdr-recovery-plan.md`: a trial of the adapter path (`DxgkDdiResetFromTimeout`,
`docs/design/tdr-recovery-s1.md`) must be able to start that path on an idle GPU, where the expected result is
known.

```
bc250tdr --list
bc250tdr --force adapter [--form int|struct] [--yes]
bc250tdr --force engine --node N [--form int|struct] [--yes]
```

| Command | What dxgkrnl does | Escape data |
|---|---|---|
| `--force adapter` | A forced TDR (`DXGK_TDR_TYPE_FORCED`): the adapter-wide reset, `DxgkDdiResetFromTimeout` | `D3DKMT_TDRDBGCTRLTYPE_FORCETDR` (0) |
| `--force engine --node N` | An engine TDR on node N: `DxgkDdiResetEngine`, and on a refusal the adapter-wide reset (TdrReason 9) | `D3DKMT_TDRDBGCTRLTYPE_ENGINETDR` (8) with `NodeOrdinal` |
| `--list` | Nothing: lists the adapters and the value of `TdrTestMode` | none |

Without `--yes` the program only lists and prints its plan (`RESULT=DRY-RUN`).

## Prerequisite

The escape is `D3DKMT_ESCAPE_TDRDBGCTRL`, which the DDI reference describes as "for testing purposes only".
dxgkrnl admits it only when the DWORD `TdrTestMode` under
`HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers` is 1 (`d3dkmthk.h`, `D3DKMT_ESCAPETYPE`). This program
reads that value and never writes it. The lab kit sets it for a trial and removes it after the trial.

The escape data has two documented forms: an `int` with the `D3DKMT_TDRDBGCTRLTYPE` value
(`PrivateDriverDataSize = sizeof(int)`), and `D3DKMT_TDRDBGCTRL_ESCAPE`, which also carries the node. `--form`
selects one. The default is `int` for an adapter TDR and `struct` for an engine TDR. Which form the lab's dxgkrnl
accepts is not measured yet. The program prints the status of the call in both cases.

## Output and exit codes

One `RESULT=` line. `TDR-REQUESTED` (exit 0) means only that `D3DKMTEscape` returned success: the TDR itself runs
asynchronously in dxgkrnl, and the kernel driver's log and `Parameters\HangRecovery` record say what happened.

| Exit | RESULT |
|---|---|
| 0 | `TDR-REQUESTED`, `LISTED`, `DRY-RUN` |
| 2 | `USAGE` |
| 3 | `NO-ADAPTER` |
| 4 | `OPEN-FAILED` |
| 5 | `ESCAPE-REFUSED status=0x...` |

## Build

```
pwsh -NoProfile -File tools\win\tdrclient\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\tdrclient
```

clang-cl from `toolchain\llvm-23.1.4-win64`, named by path, with no fallback. The build keeps the previous binary
under `retained\<sha256>.exe`, checks that two invalid command lines are refused, and prints the size and SHA-256.

Lepiej zapobiegać niż leczyć - better to prevent than to cure: a forced TDR on an idle GPU is the cheap way to learn
what the adapter path does before a real hang asks.
