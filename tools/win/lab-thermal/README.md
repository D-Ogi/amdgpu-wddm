# lab-thermal - the temperature levels of the lab tools

`thermal-levels.json` is the one definition of the temperature levels that the lab tools use. Each tool that refuses
a start or stops a run on the processor temperature (Tctl, as the kernel driver reports it) reads this file. No tool
holds a level of its own.

## The levels

The levels of 2026-11-10:

| Field | Value | Meaning |
|---|---|---|
| `driver_throttle_c` | 87 C | The kernel driver lowers the clock from this temperature. A tool does not stop a run for it. |
| `start_below_c` | 87 C | No run starts at or above this temperature. |
| `measured_start_below_c` | 80 C | A measured run (a game session, an arm of a train validation) starts only below this line. Thus the runs start from the same state. |
| `stop_held_c`, `stop_held_s` | 92 C, 10 s | A run stops when each reading for 10 s is at or above 92 C. One reading below 92 C ends the hot run. An unreadable reading does not end it. |
| `stop_at_once_c` | 97 C | A run stops at the first reading at or above 97 C. |

The table is for the reader. The file is the source: when the two are different, the file is correct and the table
is a defect.

The HLK slot has a guard of its own (94 C held 10 s). That guard is not in this repository, so it is not in this file.

## How a tool reads the levels

- A host tool in Python imports `thermal_levels.py` and calls `thermal_levels.load()`. The function refuses a file
  with an unknown schema or with levels out of order.
- A bash tool runs `python thermal_levels.py --shell` and reads the `NAME=value` lines.
- A lab script reads `thermal-levels.json` from its own directory and refuses a schema other than
  `bc250.lab-thermal-levels.v1`. The stager of the script puts the file there:
  - `target.py ps` pushes the file with each script that names it, as it pushes `kmd-reader.ps1`. A copy beside the
    script (a frozen kit) comes first.
  - `gui-trials/run-trial.py` and `kmd-deploy/stage.py` stage the file with their scripts.
  - The game trial kit outside this repository stages the file beside `lab-runner/game-runtime.ps1`.

The tools that read the levels are `lab-runner/game-runtime.ps1`, `gui-trials/gui-trial-common.ps1`,
`cts/run-batch.ps1`, `frameloop/lab/frameloop-task.ps1`, `train-validate` (the manifest, the runner and
`lab/preflight.ps1`), the preflight and postflight of `kmd-deploy`, and `lab-emerg/scripts/guard-http.sh`.

## The gate

`python -m unittest discover -s tools/win/lab-thermal` (the `lab-thermal` check of `tools/quality/quick.ps1`) does
these checks:

- The file loads, and its levels are in order.
- Each tool of the list above reads the file.
- No script or data file under `tools/` compares a temperature with a number of its own, or gives a temperature
  parameter a number of its own. Test files and test data are not scanned: they hold readings, not levels.

To change a level, change `thermal-levels.json` and the table above, and then build the kits outside this
repository again from the repository files.
