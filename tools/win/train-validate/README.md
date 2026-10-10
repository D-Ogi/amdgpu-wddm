# train-validate - the standard validation set of a release train, as one command

A release train is validated by the same set of arms every time: install the package clean, read back what
was installed, smoke the stacks, run the two owner gates, promote the D3D12 triplet, and play one game
session. Until now each train copied the previous train's shell scripts and ran the arms by hand, with an
agent between every two of them. In train b27 that cost 70 minutes of work for about 10 minutes of tests.

This tool runs the set in order with no operator between the arms:

```
python tools/win/train-validate/validate.py run --train b28 \
    --package P:\bc-250\scratch\train\b28-build\release-r1\amdgpu-wddm-tester-<version>
```

That is the whole command. It installs the package, runs every arm inside its bound, gates the machine's
health after each one, waits for the temperature to fall between arms, writes the raw log of every step and
leaves a `RESULTS.md` skeleton with one row per arm and the two owner release gates stated as met or not met.

Read the plan first. It starts nothing:

```
python tools/win/train-validate/validate.py run --train b28 --package <package dir> --dry-run
```

## What the operator still does

The suite does not take the work the owner reserved for a person.

- **The click on the Board memory card** is not part of the run. The board memory arms (`bm-*`) call the
  control application's own elevated verb, `--bc250-board-memory-action`, with a confirmation token that the
  application's own code computes from the state it reads (`lab/board-memory-op.ps1`). That is the command
  line the card starts after its confirmation dialog, so the arms cover the write, the restart and Restore,
  and not the dialog. The owner's physical CMOS clear stays the only recovery if the board does not start
  again, which is why these arms come last.
- **The interactive Witcher 3 Remaster session** (`w3-high-rt`) stays with the operator: the owner's rule is
  that the agent drives the game itself, with a half-scale shot every 10 to 15 seconds, and ends the session
  when its goals are done. The runner prints where that session fits and never starts it.
- **A value only a person can read**, such as the Rise of the Tomb Raider result screen, stays open in the
  summary, and its owner gate stays **NOT MET** until it is filled in. Fill it in afterwards without running
  anything again:

```
python tools/win/train-validate/validate.py summary --out <out dir> --package <package dir> --set rottr=55.15
```

  The reading is held to the same baseline as a parsed number: `--set rottr=12.0` is a **WARN** against the
  55.15 of b26 and the gate stays not met.

## The rules it holds

These are owner decisions, and the tool holds them in code so that they do not depend on an operator
remembering them.

| Rule | Where it is held |
|---|---|
| A trial arm lasts at most 170 s, including its cleanup. A game session lasts at most 1200 s | `arms.json` limits, held for every arm by `test_train_validate.py` |
| The thermal stop is Tctl 87 C held for 10 s, or 89 C at once | the lab runners (`pt-run.ps1`, `game-runtime.ps1`) end their own run. The suite reads the stop and marks the arm `THERMAL` |
| The machine cools before the next arm starts | `Runner.cool_down`, with the start line at 80 C and one deadline over the whole gate, the ssh calls included |
| A planned restart is over when the boot time differs and the driver reports `flags=15`, not when port 22 answers | `Runner.wait_for_boot`, after a 30 s wait and then one probe every 30 s, inside the arm's bound |
| The temperature is never polled faster than 10 s | `arms.json` limits, held by the tests. The lab sshd penalises fast probes |
| No ssh session is held open before a trial runs | one call at a time, and the only sampler is the smart plug over the LAN (BD-051) |
| The overlay STOP flag ends the run, and so does a flag that cannot be read | `Runner.stop_flag_set` wants `mon.py`'s own line (`STOP requested` or `no stop request`). Any other answer means the question did not reach the overlay, and an unread flag is never a licence to start a GPU arm |
| `bc250mon` keeps running | the suite hides the overlay for a game arm, puts it back afterwards and never stops the process. Those two calls are `optional` steps: the overlay never fails an arm |
| Every lab call goes through `target.py` | the commands in `arms.json` name no address and no host |
| A health gate follows every arm that touched the GPU | `lab/gate.ps1` and `runner.parse_gate`: GPU faults, fence timeouts, event 4101 of the display provider, bugcheck records, DPM, fan `state=curve controlling=1`, TdrDelay 10 |
| A failed arm keeps its logs, restores what it changed, ends its client on the lab and does not stop the independent arms | the `post` steps of an arm run on every exit path, then `lab/kill-clients.ps1` for an arm that names a client. `depends_on` decides what is skipped |
| The uninstall does not run on an unproved package | `needs_verified` on the arm: the destructive step waits until `zipcheck` has proved the zip on the lab in this run. A failed push leaves the lab with its driver, not with a Microsoft Basic Display Adapter and a person to call |
| A baselined arm that read no number is a failure, not a pass | `runner.judge`: no number is `UNREAD`, and an owner gate is met only when every arm of it passed **and** its value was read and compared |
| The suite never diagnoses | the summary names the symptom shape of a known failure class and says that a shape is not a diagnosis |

A bugcheck record or a health flag other than 15 after an arm stops the run: the machine then needs a person,
not another arm. Two refused ssh connections stop it as well, rather than feed the connection penalty. The
refusals are counted over every ssh call the runner makes, the gates between the arms included. They are not
counted while a planned restart is in progress, where a refusal is the expected answer.

## Who owns which bound

The owner's three-minute trial bound is held on the **lab** side, and the host timeout is a backstop over it.
The difference matters when a number is read out of this table.

| Quantity | Value | Who holds it |
|---|---|---|
| A trial's own run | 150 s or 160 s, as the arm asks | the lab script: `pt-run.ps1` registers its client as a scheduled task with `ExecutionTimeLimit = Seconds + 30`, so even an abandoned ssh call cannot leave the GPU running |
| The arm's declared bound | 170 s for a trial, 1200 s for a game | `arms.json`, held by the tests |
| The host backstop of one step | bound + `host_grace_s` (45 s), so 215 s for a trial | `manifest.plan_steps`. It is over the owner's 180 s on purpose: it must never fire before the lab's own end |
| The arm's worst case on this side | the sum of its step timeouts, printed per arm by `--dry-run` | a promote arm issues up to nine steps, so its worst case is some twenty minutes although each lab step is inside the trial bound |
| The plug sampler's join after an arm | up to 40 s | `Runner.run_arm` |
| The cool-down before a trial or game arm | up to `tctl_cool_max_s` (420 s) plus one read (90 s at worst) | `Runner.cool_down`: one deadline over the reads and the waits together |

## The arms

`validate.py arms` prints the set. The order in `arms.json` is the running order.

| Arm | Kind | What it answers |
|---|---|---|
| `push-zip`, `zipcheck` | transfer, read | the package on the lab is the package on this PC |
| `uninstall`, `restart-1`, `install`, `restart-2` | lab | the owner's installer test: nothing of ours is left, then a fresh install at 40 CU |
| `slots`, `kmdver`, `preflight` | read | every installed file against the package manifest, the identity of what runs, and 40 CU, DPM, health and TdrDelay |
| `pin-baseline` | host | the trial harness is pinned to this package. `pin-baseline.py` wraps `release-baseline.py --apply`. That script refuses a second apply of the same release. The wrapper then reads `lab-baseline.json`. It passes when that file already names this release and this manifest hash. A resumed run thus gets the same answer as the first run. The script also refuses a package with a new D3D12 triplet. The wrapper then applies it again with `--keep-accepted-d3d12`. The D3D12 block keeps the accepted triplet until `d3d12-promote` replaces it |
| `stage-clients` | transfer | the clients that are not in the package are on the lab with the bytes of this PC. `stage.py` hashes every file on both sides and sends only what differs |
| `vk-smoke`, `x86-smoke` | lab | the system Vulkan ICD and the 32-bit D3D11 stack answer |
| `vkheaps` | read | the Vulkan memory heaps of the installed system ICD follow the board's carve-out, not a frozen capture |
| `vk-semaphore` | lab | a Win32 export and import of a timeline semaphore across two processes, unnamed and named, in session 0 and in the interactive session (`tools/win/vksemcheck`). The 32-bit run is a control that decides nothing |
| `q2rtx-pipeline`, `q2rtx-query`, `q2rtx-loop` | lab, **owner gate** | Quake II RTX runs with `VK_KHR_ray_tracing_pipeline` and with `VK_KHR_ray_query` |
| `cache-0`, `cache-1`, `q2rtx-warm`, `cache-2` | read, lab | the shader disk cache of a new Vulkan driver build is written and then read. A new build has a cache namespace of its own (BD-100), so this is the b25 B1 proof again |
| `dxrpt` | lab | path tracing through D3D12 holds its frame rate |
| `cts-rt` | lab | the 140 ray tracing cases of the pinned Vulkan CTS, on the installed system driver |
| `llm-dense` | lab | the dense language model batch that stopped Windows with 0x116 runs to its end (BD-114) |
| `llm-q35` | lab | the 35B-A3B IQ2_M model runs fully offloaded at the 12 GiB carve-out |
| `hip` | lab | the installed HIP runtime is on the machine PATH, `vadd.exe` computes, and the HIP llama.cpp backend runs `llama-bench` through it (BD-110) |
| `d3d12-promote` | promote | the package D3D12 triplet becomes the registered one, with its witness. For a new triplet it first puts the accepted files back in the install directory, because the attempt verifies them before it swaps (`lab/d3d12-putback.ps1`). The release file waits next to the accepted one as `<name>.release` until the promotion has put it back |
| `rottr` | game, **owner gate** | the Rise of the Tomb Raider benchmark completes every scene |
| `w3-high-rt` | operator | the Witcher 3 Remaster at HIGH with ray tracing, driven by the operator |
| `bm-before`, `bm-set-8192`, `bm-restart-1`, `bm-at-8192`, `vkheaps-8192`, `bm-restore`, `bm-restart-2`, `bm-after` | read, lab, restart | the board memory operation of the control application. It sets 8192 MiB and restarts. It reads the active size, Windows RAM and the Vulkan heaps at 8192 MiB. Then Restore and a restart put back the size before the change |

Subsets run by name: `--arms install`, `--arms gates`, `--arms smoke`, `--arms cache`, `--arms llm`,
`--arms hip`, `--arms semaphore`, `--arms board-memory`, or a list such as `--arms q2rtx-pipeline,q2rtx-query`.

Some arms stand on lab staging that is not part of this kit, and each one says so when it is absent:
`cts-rt` needs the CTS package at `C:\BC250\cts`. `llm-dense` and `llm-q35` need the seg0 supervisor kit, the
models and the llama.cpp build where the BD-114 round and the LLM work left them, under `C:\BC250\bd114`,
`C:\BC250\llmvram` and `C:\BC250\strata\models`. `hip` uses the same supervisor kit. The arm `stage-clients`
sends what the `vk-semaphore` and `hip` arms run.

## The next train

Two files change between trains, and no script has to be copied.

1. `arms.json`: the baseline of an arm, which is the value the previous train measured, with its tolerance
   and the run it comes from. An arm whose value falls under the tolerance is a **WARN**, not a failure: a
   number below a baseline needs a reader, and the suite does not decide for them. An arm that read **no**
   number is a different matter: it is `UNREAD`, the arms that depend on it do not run, and an owner gate
   over it is not met. A baseline is pinned to a run that passed, never to one that hit its bound.
2. The package directory on the command line.

A new arm is an entry in `arms.json`. It names a command that already exists, its bound, its kind, the
regular expression that reads its result line and its baseline. The tests then hold it to the owner's bounds,
and they refuse an arm whose lab script is absent or does not declare the parameters the manifest passes.

## What this replaces

Where two pieces of the workspace did the same job, the suite keeps one of them.

| Kept | Dropped | Why |
|---|---|---|
| `lab/*.ps1` here, with the package as an argument | `scratch/train/bNN/validation/ps/clean-slate-bNN.ps1`, `slots-bNN.ps1`, `kmdver-bNN.ps1`, `zipcheck.ps1` | the b27 copies were already parameterised. The train name in the file name was all that made them per-train |
| `lab/gate.ps1` | `gatecheck.ps1` and `tdr-check.ps1` of each train | one gate, one output shape, parsed on this side. It also reads the fan line, which the old gate did not |
| `arms.json` plus `validate.py` | `run-bNN-rottr.sh`, `run-bNN-w3.sh` | the per-train wrappers held the game session's environment, the plug sampler and the closure steps in shell. They are data here |
| `scratch/m15/native-caps001/run-game.sh`, `run-m157.sh`, `run-slot.py`, `promote-d3d12.py`, `release-baseline.py` | a second runner of our own | the native harness owns the attempt, the witness and the rollback, and it is proven. The suite drives it |
| `scratch/pathtrace/lab/pt-run.ps1` | a second path tracing runner | it already holds the thermal stop, the bound and the task start in the console session |
| the smart plug sampler of `scratch/seg0-cpu-write/lab/kit/run-arm2.py` | the ssh-side samplers of the older kits | power over the LAN needs no ssh session, so it cannot collide with a trial |

## Files

| File | What it is |
|---|---|
| `arms.json` | the arms, their bounds, their result lines, their baselines, the limits and the known failure classes |
| `validate.py` | the command: `run`, `run --dry-run`, `summary`, `arms` |
| `manifest.py` | reads `arms.json` and the package, resolves the placeholders, builds the plan |
| `runner.py` | the arm loop: the bounds, the STOP flag, the temperature gate, the health gate, the raw records |
| `promote.py` | the D3D12 promotion step, idempotent. It moves a leftover attempt directory aside and leaves evidence alone. For a new triplet it finds the local copy of each accepted file that the package replaces |
| `summary.py` | the `RESULTS.md` skeleton, the two owner gates and the symptom shapes |
| `stage.py` | sends files to the lab and proves them by SHA-256 on both sides |
| `lab/*.ps1` | the lab side, all of it parameterised by the package. Read-only but for `clean-slate.ps1` (the owner's installer test), `restart-now.ps1`, `kill-clients.ps1` (the client of a failed arm) and `board-memory-op.ps1 -Step set` and `-Step restore` (the board's memory block). `d3d12-putback.ps1` writes too: it puts the accepted D3D12 files back before a promotion |
| `test_train_validate.py` | the host tests, against a fake target |

## Commands

```
python tools/win/train-validate/validate.py arms                          the arms and the sets
python tools/win/train-validate/validate.py run --package <dir> --dry-run the plan, nothing starts
python tools/win/train-validate/validate.py run --package <dir>           the whole set
python tools/win/train-validate/validate.py run --package <dir> --arms gates --resume
python tools/win/train-validate/validate.py summary --out <dir> --package <dir> --set rottr=55.15
python -m unittest discover -s tools/win/train-validate                   the host tests
```

The host tests run the whole sequence against a fake target and a fake clock, with no lab and no sleeping:
an arm that overruns its bound, a thermal stop, a set STOP flag, a STOP flag that cannot be read, a failed
health gate, a leftover attempt directory, a destructive arm asked for without its proof, a baselined arm
that read nothing, a lab that answers every temperature read after 90 s, a refusal between two arms and a
clean run of the whole shipped set. `tools/quality/quick.ps1` runs them under the name `train-validate`.

Wolniej, a dokladniej, czy szybciej i raz? Jedno nie musi wykluczac drugiego.
("Slower but exact, or faster and once? The two need not exclude each other.")
