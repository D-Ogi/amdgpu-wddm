# Static checks of the kernel-mode driver: kmd-tidy and kmd-ism

Two gates of `tools/quality/quick.ps1` read the C code of `driver/kmd` and `driver/shim` with clang. Both read the
compile database that the `kmd-compile` gate writes (`<out>/kmd/compile_commands.json`). Both replay the cl.exe
commands of `driver/kmd/build.ps1` with clang-cl (`tools/quality/kmd_clang.py`).

| Gate | Tool | What it reports | Baseline |
|---|---|---|---|
| `kmd-tidy` | `tools/quality/kmd_tidy.py` | clang-tidy findings of the checks in `driver/kmd/.clang-tidy` | `tools/quality/kmd_tidy_baseline.txt` |
| `kmd-ism` | `tools/quality/kmd_ism.py` | candidates for an implicit state machine | `tools/quality/kmd_ism_baseline.txt` |

Both gates are ratchets. The baseline records the findings of the day that the gate started. A finding that is
not in the baseline fails the gate. A finding in the baseline does not fail the gate. No key contains a line
number, so an edit above a finding does not move the finding out of its baseline line.

## Run a gate by hand

1. Write the compile database: `pwsh driver/kmd/build.ps1 -Kits <kits> -Out <dir> -ExportCommandsOnly`.
2. Run clang-tidy: `python tools/quality/kmd_tidy.py --database <dir>/compile_commands.json --out <report dir>`.
3. Run the detector: `python tools/quality/kmd_ism.py --database <dir>/compile_commands.json --out <report dir>`.

The `--jobs` option sets the number of clang processes. The default is 4.

## Read a kmd-tidy finding

The gate prints each new finding with its file, line, column, check name and message. The file `report.json` in
the output directory has the counts per check. The file `findings.json` has each finding.

`driver/kmd/.clang-tidy` lists the checks. It also gives the reason for each check that it disables. The checks
look for state defects. Examples are a value that nothing reads (`clang-analyzer-deadcode.DeadStores`), two
branches that do the same thing (`bugprone-branch-clone`), and a switch without a case for each enum value
(`clang-diagnostic-switch-enum`).

When clang cannot compile a translation unit, the gate fails and prints the clang error. A gate that cannot read
the code cannot judge it. `tools/quality/kmd_clang_prelude.h` removes one known WDK header problem for clang.

## Read a kmd-ism candidate

An implicit state machine is a state that no single type names. The file `report.txt` in the output directory
lists the candidates in five groups:

| Kind | The detector reports | What to examine |
|---|---|---|
| `cluster A B` | Two state fields that the same functions write, or that one function writes and one condition reads together. Two fields of different structs make a pair only along a call. | Do the two fields together make one phase? If yes, one enum can replace them. |
| `dead A` | A field that the driver writes and never reads. | Remove the field, or add the reader that the comments expect. |
| `sticky A` | A flag that the driver sets to a value that is not zero, and that only start or init functions clear. | Is a restart the only exit from this state? Is that the intent? |
| `int-state A` | An integer field that the driver writes with three or more constants and with nothing else. | Give the values an enum. |
| `switch F E` | A switch in function F over enum E without a case for each value. | Add the cases, or state why the default is right. |

The detector examines only the structs that hold lasting state. These are the device extension, each struct that
it has or points to, and each struct at file scope. Structs of the escape interface, journals and progress records
are out of scope for `dead`, because readers outside the driver read them.

Each candidate is a question for a person, not a defect. The tools find candidates. People judge them.

## Positive controls of kmd-ism

`tools/quality/kmd_ism_controls.txt` lists candidates that a person examined and found to be hidden state. The gate fails when
one of them is not in its report, because then the detector cannot see a known case. When a fix removes the hidden
state of a control, the same change moves its line below `## fixed`.

`tools/quality/test_kmd_ism.py` holds two more controls. `kmd_ism_fixtures/clean_state.c` is one explicit enum
state machine, and the detector must report nothing in it. `kmd_ism_fixtures/loose_flags.c` is the same machine
with loose fields, and the detector must report one candidate of each kind.

## Accept a finding into the baseline

Fix the code first. Accept a finding only when the code is right as it is.

1. Run the gate with `--accept`. The tool writes each finding of today into its baseline.
2. Read the difference of the baseline file in the change. Each new line must be a finding that you examined.
3. Write the reason in the commit message.

After a fix, run the gate with `--prune`. The tool removes the lines that the code does not have any more. A
baseline line that the code does not have is a note, not a failure.

## Findings that a person examined

The table records the result of each examination. The baseline keeps the findings that stay in the code. The
reason is here, because a baseline line has no space for it.

| Finding | Result | Reason |
|---|---|---|
| `clang-analyzer-core.NullDereference` in `connector_link.c`, `Begin` from `ConnectorLinkPrepare` | Not a defect | Each `goto failed` path of `OpenContext` keeps an error status, and `OpenResult` returns that status. The analyzer does not follow `OpenResult`, so it assumes a success. |
| `clang-analyzer-core.NullDereference` in `wddm.c`, the `Pitch` writes of the shadow, staging and GDI surfaces | Not a defect | `Bc250StdAllocDecide` refuses a request whose union member is NULL (`gdi_admission.h`). The function returns before the `Pitch` writes. |
| `clang-analyzer-core.NullDereference` in `wddm.c`, `object->Allocation = *private` | Not a defect | `Bc250Lb7aAdmit` gives `BC250_LB7A_UNREAD` for a NULL blob. Only `BC250_LB7A_ADMITTED` gets to the copy. |
| `clang-analyzer-core.UndefinedBinaryOperatorResult` in `bc250_cu_mode.c`, `write_and_verify` | Not a defect | The start sets `sa_count` to `se_count * sh_per_se`. The caller fills each of those entries before the call. |
| `sticky _DCN_CURSOR.Stopped` | Intended | `Stopped` ends the cursor for one start. `DcnCursorRelease` frees the object, and the next `DcnCursorPrepare` makes a new object with all fields zero. |
| `cluster` of the writer fields of `_BC250_WDDM` (`WriterActive`, `WriterReady`, `WriterResumed`, `WriterDeposit`, `DesiredOwed`) | Explicit machine | `flip_state.h` holds the edge table, and `docs/design/vidpn-flip.md` section 7.2 holds the design. All fields change under the lock L. `WddmPrimaryExclusiveBegin` and `WddmPrimaryExclusiveEnd` set all of them together. |
| `int-state CONNECTOR_LINK.Phase` | Not state | `Phase` is a progress mark for the log and the failure record. No condition reads it. |
| `dead _BC250_DEVICE.RuntimePlaneBlank` | Removed | The driver wrote the field in eight places and read it in none. `DcnStop` reads the plane blank from `HUBP0_DCHUBP_CNTL`. |
| `dead BC250_HDMI_LINK.Restoring` | Removed | `Bc250HdmiLinkRestore` set and cleared the flag. No code read it. The restore writes go through `Io.Write`, not through the journal. |
| `dead _BC250_WDDM.PagingDeferredFence` and the clusters of `PagingDeferredValid` | Removed | `WddmCompleteSoftware` returns before it can set them, because node 1 sends its software fence through the paging FIFO. `PagingDeferredValid` was always FALSE, but the power-idle check still read it. |
