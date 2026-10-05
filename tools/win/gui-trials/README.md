# gui-trials - six bounded lab trials of the control application

The control application (`tools/win/amdgpu_wddm_control`) is built and tested on the development PC, but its
acceptance happens on the lab unit, in an interactive session, with a real driver under it. These six kits run
one bounded stage each on unit A and bring back evidence. They are infrastructure: a kit proves that a stage
ran inside its bounds and closed, and a human then reads the screenshots against the pass criteria in
`T2.md`. No kit converts a clean process exit into a correct window.

The kits are ready. No kit has run on the lab yet.

| Kit | Stage |
|---|---|
| `L1.ps1` | The window opens and shows its pages (`view`) |
| `L3.ps1` | Start and one click (`startup-and-click`) |
| `L4.ps1` | A stage with and one without the prepared state |
| `L5.ps1` | The stages `upgrade`, `resume`, `repair` and `verify` |
| `L-CU.ps1` | The compute-unit choice: 24, a request for 40, the observation, the confirmation, and back |
| `L-OFF.ps1` | The stage `repair` with the network closed off |

`gui-trial-common.ps1` holds the one entry point behind every kit (the modes `Prepare`, `Start`, `Observe`,
`Stop`, `Cleanup`, `Supervise`, `Worker` and `Guard`), and `gui-trial-firewall.ps1` holds the offline lease of
`L-OFF`. `run-trial.py` is the host companion, `T2.md` the admission and the pass criteria, `HOST.txt` the
host contract, `config.example.json` a template that is not runnable, and `PS-PARSE.json` the parse receipt of
the eight scripts.

## The review oracles are not here, on purpose

`G-CU`, `G-VER` and `G-ART` check the control application against three oracles that the reviewer wrote from
the plan alone. The oracles stay outside this repository and are read where they lie
(`AMDGPU_WDDM_ORACLE`, which `tools/win/amdgpu_wddm_control/build.ps1 -Oracle` sets). A copy in the
repository would let the implementation be fitted to the oracle, which is the one thing an oracle must
prevent. The directory and the three SHA-256 values are recorded in the control application's README, under
"Review oracles", so that a lost path no longer makes the three checks skip in silence.

## How to run it

```
python tools/win/gui-trials/run-trial.py --config <candidate>.json --check     the local check only
python tools/win/gui-trials/run-trial.py --config <candidate>.json --execute   the lead operator only
python -m unittest discover -s tools/win/gui-trials -p test_run_trial.py       15 host tests, no lab
pwsh -NoProfile -File tools/win/gui-trials/validate-T2.ps1                     53 static checks, no lab
```

Without `--execute` the host only parses the configuration. It builds no transport, reads no credential,
queries no device and opens no window. The `gui-trials` check of `tools/quality/quick.ps1` runs both test
commands.

`--execute` copies the chosen kit entry, the two helpers and the configuration to the lab through
`tools/win/target.py`, reserves a new remote directory, and then drives the modes. The artifact paths in a
configuration are lab paths of files that are already staged. Results go to the workspace scratch directory
(`BC250_GUI_TRIAL_RUNS` names another directory outside this repository), one new directory per trial
identifier.

## What a stage needs

- A filled configuration from `config.example.json`, with the lab computer name, the interactive session
  identifier, the user SID, and a SHA-256 for the bounded helper and for the reviewed scenario adapter. The
  template is a template: every placeholder must go.
- The scenario adapter itself, which this kit does not supply.
- A duration between 45 s and 180 s. The lab owns that deadline, with its own STOP flag and its own thermal
  guard, and the host cannot extend it.
- For `L-CU`: the smart plug for the wall power (`tools/win/smartplug`) and the independent lab temperature
  guard. A missing power reading fails the kit. The plug's readings are not calibrated for this model and
  their age is unknown, so they are kept raw, and the temperature never comes from them.
- For a mutating stage (`L4`, `L5`, `L-OFF`): the engine's cancellation and closure admission, and a closure
  receipt per run. Until that contract exists, these stages stay refused.

## Hazards

- **A passing local check authorises nothing.** It checks the configuration, and not the remote paths, the
  hashes, the admission or the scenario.
- **Uncertain closure is `recovery-required`, never a pass.** `Stop` is asynchronous: the host asks for it,
  waits at most 20 s for the closure, and only then cleans up. `Cleanup` needs the explicit acknowledgement.
- **A remote directory is never reused.** The host reserves it with `New-Item` and no `-Force`, so an existing
  trial directory refuses the stage before a copy can overwrite its evidence. Use a new identifier and a new
  configuration for every stage.
- **Every image is at half scale**, taken through the overlay, and the host takes one only after the lab has
  proved that the stage runs.
- **`host-result.json` always says `acceptance_pass: false`** and asks for the operator checklist.
- The kit does not restart Windows, does not restart the compositor, and does not install an artifact. A
  forced restart of the compositor leaves WinUI content without mouse input until the next boot (BD-060).
- `L-OFF` blocks everything but the local subnets, with two rules that it owns by name, group and description,
  and it changes no default firewall profile. It removes only a rule that is exactly its own.

## The scratch copy

The reviewer's working directory stays in the workspace scratch directory, with the oracles, the trial
history and the per-task question files. This copy is the source of record for the kits themselves.
