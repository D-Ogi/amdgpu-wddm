# kmd-deploy - the standard way to put a kernel driver on unit A

One tool for every KMD promotion and for its automatic rollback, parameterized by two package directories. It
replaces the KMD on a machine the operator cannot see, inside a bounded task, and puts the previous driver back
without help when anything fails. Every gate of the measured 0.7.173.1 promotion is still here. Nothing
version-specific is left in the scripts.

Only the steps marked LAB touch unit A. Every lab trial stays within three minutes.

## Inputs

- `--package DIR`: the candidate. A directory with `bc250kmd.sys`, `.inf`, `.cat`, `bc250-lab-test.cer` and the
  build's `source-manifest.json`. The build must be BUILT, deployment-source eligible and clean, from
  `driver/kmd/build.ps1` on a clean checkout, in the plain `package` flavor.
- `--rollback DIR`: the KMD deployed now, in the same form. Either the candidate directory of the attempt that
  promoted it, or its build package directory. When the lab runs a tester release, give the release package
  directory (`manifest.json`, `payload/kmd`, `payload/cert`). See "A release package as the rollback".
- `--rollback-build DIR`: only with a release package as `--rollback`. The build package directory that the
  release packages: the `source` of `payload/kmd` in the release's `release-sources.json`.
- `--repo DIR`: the git repository that holds both commits. The default is `<BC250_ROOT>/bc250-win`.

Nothing above is typed twice. `freeze` reads the version from `DriverVer`, the ABI from `BC250_KMD_VERSION` at
the commit, the SYS/INF/CAT hashes from the files, the detector contract from the INF and the hang settings from
`hang.c`. The rules for the version are in the next section.

## Versions and labels

The identity of a package is (R, B) from `DriverVer` 0.7.R.B:

- R is the ABI revision. It must be the low 16 bits of `BC250_KMD_VERSION` at the commit (0.7.216.B and
  0x000700D8). `freeze` refuses a version and an ABI that name different revisions.
- B is the build counter, a decimal number of 1 or more. Up to 0.7.215 every build was 0.7.R.1. From 0.7.216 the
  counter goes up with each build (0.7.216.14, 0.7.216.16). A release package has a B of its own (0.7.216.100).
- The package INF and the source INF at the commit must name the same `DriverVer`. The only exception is a
  release package, under the rule of the next section.

The candidate and the rollback must differ in the full version (R, B) and in the SYS hash. The lab arms tell the two
drivers apart by `DriverVer` and by the SYS hash. Two builds of one revision have the same ABI, so the ABI alone
does not tell them apart. One package as both candidate and rollback (the same-package control) is never frozen.

The labels and the attempt name carry (R, B). A build 1 keeps the old names: `candidate175`, `rollback173`,
`kmd175-deploy001`. A build B of 2 or more adds `-B`: `candidate216-16`, `rollback216-100`, `kmd216-16-deploy001`.
The kit never writes `-1`, so one (R, B) has exactly one name. Attempts frozen with the old rule keep their names
and still work with every step.

## A release package as the rollback

`tools/release/build-release.ps1` packages a build and changes it in a known way:

1. The INF gets the release's own `DriverVer` (manifest.json `kmd_version`, for example 0.7.216.100). The first
   three fields stay those of the build (manifest.json `kmd_build`, for example 0.7.216.14).
2. The INF gets one `Reboot` line after each install section header (`Add-InfRebootDirective`).
3. The release signs the SYS again with the release certificate and makes a new catalog. The code does not change.

`freeze` accepts the release package only when each of these checks passes:

- Every file in `payload/kmd` and the release certificate has its hash in `manifest.json`.
- The release INF names manifest.json `kmd_version`. The build INF names manifest.json `kmd_build`, and both name
  the same 0.7.R.
- The release INF without the two edits above is the build INF, character for character.
- The release SYS and the build SYS have the same image digest (the SHA256 without the signature).
- manifest.json `kmd_abi` is the ABI of the build commit.
- The certificate that manifest.json names signs the SYS and the CAT. The candidate keeps the lab certificate.
- `lab-baseline.json` names this KMD, and its `release` block names this manifest and this build.

The attempt copies the release SYS, INF, CAT and certificate into `rollbackRRR-B`, together with
`release-manifest.json` and `build-source-manifest.json`.

The release INF has the `Reboot` directive. Up to 2026-10-07 no attempt restored a release package on the lab.
Thus a restore that needs a restart before the device takes the release driver is possible and not measured. Do the
first promotion over a release as a `--mode rehearsal` attempt.

`<BC250_ROOT>/scratch/m15/native-caps001/lab-baseline.json` is the deployed baseline. The rollback must be the
KMD it names, and it supplies the desktop UMD and ICD pins that `preflight`, `Verify` and `postflight` require.

## The installed release

The lab runs a release install since tester.10 (owner, 2026-10-03), and the kit reads the lab from that install
alone. The release installer writes `InstallRoot` into `HKLM\SOFTWARE\amdgpu-wddm\Release`.

- The KMD client is `<InstallRoot>\tools\bc250kmd_cli.exe`, and no other copy is used.
  `kmd-transition/release.ps1` finds it and reads the client's own usage for each query form the caller needs
  (`info`, `health read`, `health confirm`, `clock read`, `log`, `log summary`). A release that is not installed, a
  client that is not there, and a form the usage does not name are refused, each with its cause. Every `ops`
  runner holds the same rule in one line, because each of them runs alone on the lab.
- The desktop UMD, the system ICD and the desktop route come from `lab-baseline.json`, which `release-baseline.py`
  derives from the release manifest. `freeze` writes them into the attempt's own `identity.ps1`. No file of the kit
  holds the hash of one release.
- `lab-baseline.json` must have its `desktop` block. The release installer registers the desktop router, so a
  baseline without that block cannot describe the lab, and `freeze` refuses it.
- `check-offline.py` has the check `legacy lab paths`. No file of the kit may name `C:\BC250\m8` to `C:\BC250\m14`,
  and no file may name a KMD client by its path. Those lab directories go away (owner, 2026-10-08), and a second
  client copy can answer about a driver other than the one on the GPU.

`freeze` writes each attempt's own `kmd-transition/identity.ps1` from these inputs and then runs
`test-identity.ps1` on it: no version, ABI, label, hash or lab path literal may appear in any other script. The
template's `identity.ps1` is only a host-test fixture. Attempts live in
`<BC250_ROOT>/scratch/kmd-deploy/attempts/kmdRRR-deployNNN` (build 1) or `kmdRRR-B-deployNNN` and are never
rewritten. Any change is a new freeze. The steps below write `kmdRRR-deployNNN` for both forms.

## Steps

| # | Command | Pass |
|---|---------|------|
| 1 | `python check-offline.py` | 44/44. `--quick` 41/41, measured 2026-10-08 from this copy |
| 2 | `python stage.py freeze --package <dir> --rollback <dir>` (`--rollback-build <dir>` with a release package, `--mode rehearsal` always rolls back) | prints the attempt name and the manifest hash |
| 3 | LAB `mon.py status "KMD<R> promotion: staging" info`, then `mon.py stop?` | STOP clear |
| 4 | LAB `python stage.py push kmdRRR-deployNNN` | exit 0: a 30 s bounded child checks the rollback baseline, and the candidate enters the DriverStore without an install |
| 5 | LAB `python dispatch.py kmdRRR-deployNNN Prepare`, `mon.py status "... 3 min" warn`, `python dispatch.py kmdRRR-deployNNN Start` | task `BC250-KMD-Watch` Ready. Start first starts the present heartbeat and refuses if it does not run |
| 6 | LAB `python dispatch.py kmdRRR-deployNNN Inspect` every 20 to 30 s, or `ops\inspect-until-done.ps1 -Attempt kmdRRR-deployNNN` | `state` not Running, and a result |
| 7 | LAB `python collect.py kmdRRR-deployNNN` | `closed` and `candidate_retained: true` |
| 8 | LAB `python dispatch.py kmdRRR-deployNNN Cleanup` **twice**, then `Inspect` and `collect.py` again | `state` Missing |
| 9 | LAB `python postflight.py kmdRRR-deployNNN` | exit 0, receipt `postflight-N.json`. It starts its own 60 s heartbeat and stops it afterwards |
| 10 | `python accept.py kmdRRR-deployNNN`, then `--apply` | prints the `lab-baseline.json` and `STATE.md` edits and the next `--rollback`. `--apply` writes `lab-baseline.json` |

Recovery, the rollback semantics and the result table are those of the kmd173 runbook in the workspace, with
`BC250-KMD173-Watch` read as `BC250-KMD-Watch`, `kmd173-transition` as `kmd-transition` and `preflight171.ps1`
as `preflight.ps1`. The optional fresh-DWM pre-step is `ops\fresh-dwm-run.ps1`.

## The lessons, each paid for by a failed attempt

**A candidate over an installed release.** `<BC250_ROOT>\scratch\dp-audio\deploy-candidate.ps1` is the by-hand
route: it puts a candidate on the GPU over the release install in three steps, and each step names a trap the kit
must answer.

1. It installs with `UpdateDriverForPlugAndPlayDevices` and `INSTALLFLAG_FORCE`, because the release INF has a
   higher version than a candidate and wins the rank. The kit does not rank at all. `select-driver.exe
   --install-deferred` builds the driver list from the candidate INF alone (`DI_ENUMSINGLEINF`), it needs exactly
   one compatible node, and it selects that node on the disabled adapter.
2. It applies `registry-defaults.json` again after the install, because an INF install closed every gate the INF
   names (BD-091). The `Configure` phase writes the whole capture of `Parameters` back, which covers that shape and
   any other. It records what the install changed in `<receipt>-parameters-after-install.json`, and it writes
   `UnconfirmedStarts` 0, so the candidate always starts with a fresh start budget (BD-090).
3. It then restarts Windows and never the device, because a live device restart over the release ends at Code 43
   (BD-090). The kit does not restart the live device either. It disables the adapter, installs, configures and
   enables it, and it runs on the CPU desktop route with the present heartbeat, which is the path measured since
   revision 173. An attempt that needs a restart ends `recovery-required`, and the by-hand steps below put the lab
   back.

**The candidate budget is 122 s, not 107 s.** A candidate is retained only when its start health reaches
`ready_ms >= 60000` inside the candidate `Verify` phase. kmd179-deploy002 failed because the driver enable took 15 s
once (1.9 s the next time, 3 s for 178), so the 60 s readiness missed the eligibility bound by one second. The
budgets now: candidate end 122 s, `Verify` cap 85 s, `CandidateSeconds` 125. The `Verify` phase saves the driver's
log ring as `candidate-verify-kmdlog.txt`.

**Run `Cleanup` twice before `accept`.** `accept.py` needs the newest Cleanup receipt to say state Missing. The
first Cleanup prints the state before the task is unregistered, which is "Ready". Then `collect.py`,
`postflight.py`, `accept.py`, `accept.py --apply`.

**A PowerShell `REG_DWORD` above 0x7FFFFFFF reads as a negative `[int]`.** Compare it masked
(`-band 0xFFFFFFFF`). Trial todpm-1 stopped on exactly this.

**`target.py wait` returns at once after `shutdown /r`,** because the machine has not gone down yet. Wait for a
boot time newer than the one you recorded (`wait-new-boot.py` in the workspace cu-trial directory probes every
30 s, never per second: the lab's sshd penalises that). After any boot an operator must check the start by hand,
otherwise an unconfirmed 40 CU or DPM request falls back at the next start.

**A `recovery-required` or `restore-unverified` end needs by-hand steps**, in this order: check the health of
the restored KMD (`ops\heartbeat-start.ps1 -Seconds N`, then `ops\confirm-current.ps1`), then
`dispatch.py <attempt> RestoreDetector`, then `target.py ps ops\unregister-task.ps1`. `Prepare` of the next
attempt refuses while the old task exists.

**A candidate that crashed the machine leaves the kit refusing "Boot/host changed".** Recover by hand with
`ops\recover-after-boot.ps1 -Attempt <attempt>`.

**`freeze` refuses a build whose `source-manifest.json` is not `source_clean`.** Never edit an INF or a source
file in the worktree with `sed -i`: MSYS `sed` rewrites CRLF to LF and git then sees the whole file modified.
Edit with Python, commit, rebuild.

**The CU and DPM trials bind to a package, not to the deployed driver.** `cu-trial.py` and `dpm-trial.py` take
`BC250_KMD_PACKAGE`, and `dpm-trial.py` also `BC250_KMD_REPO`, because the commit of a new candidate is often
not in the worktree their defaults name. Set both after a promotion, or the trial measures the wrong revision.

## The present heartbeat

Every confirmation (candidate `Verify`, restore `Verify`, `postflight`) needs a fresh start-health witness: completed
GPU work no older than 15 s. An idle desktop presents about once a minute, so without a presenter the
confirmation fails and the attempt ends `recovery-required` or `restore-unverified`. That is kmd175-deploy002.

The presenter is `ops\heartbeat.ps1`: a small top-most window in the owner's session whose text changes every
250 ms, started as the interactive user by the one-shot task `Lab-Present-Heartbeat`, which exits by itself.
`dispatch Start` and `postflight` start it themselves. Its task name must stay outside the `BC250|DWM|G0|WSI`
filter of the competing-task gates, and `kmdcommon.heartbeat` refuses if it does not.

**A mode change after `Verify` restarts the ready clock, and that is not a refusal.** Since BD-098 the
`CONFIRMED` flag follows the start, that is its generation, while every mode set and every visibility change
advances the epoch and sets `ready_ms` back to zero (`driver/kmd/start_health.c`, `HealthInvalidate`). A game's
exit mode commit therefore leaves flags 15 with a ready age of a few seconds, and the old rule
`confirmed and ready_ms < 60000` refused it. W3 483, 484 and 485 of the b23 validation closed
`recovery-unverified` on this rule alone. The Ascent 489 closed the same way on another rule for the same epoch
advance, the epoch equality of the game kit's own health comparison, which is a different caller and is not
fixed here. `Get-ConfirmedPresentStart` now takes `-ConfirmedEpoch`, the epoch
of a witness of the same start that a caller already admitted with the full ready age, together with
`-ExpectedGeneration`. A later epoch of that generation may then carry a short ready age. `postflight` reads
that epoch from this attempt's own `*-health-acceptance.json` receipts (`Get-KmdAcceptedConfirmedEpoch`) and
refuses when no record names the live generation. A new generation, a witness older than 15 s, an unknown flag
bit and a wrong ABI are refused as before. The kit serves every KMD revision, and on one older than BD-098 the
same mode commit clears `CONFIRMED` instead of advancing the epoch alone: the witness then reads flags 7, the
relaxation does not apply and the refusal is `Confirmed CPU baseline required`, as it was under tester.20.

## bounded-child output files

`bounded-child.exe` creates its stdout and stderr files with `CREATE_NEW` and exits 125 before starting anything
when they already exist. The five postflights of kmd175-deploy003 all ended 125 without running and printed the
first run's stale message. Every ops runner therefore writes into its own UTC-stamped directory.

## Driver parameters in postflight

The driver writes some `Parameters` values itself, at every start or while it runs: `UnconfirmedStarts`,
`LastStage`, `StageHistory`, `KeepStatus`, `CuModeLastApplied`, `CuModeLastReason`, `DpmLastMode`,
`DpmLastReason`, `DpmSession`. They differ from the capture whenever a start ends differently, and
kmd178-deploy001 failed its postflight on `CuModeLastReason` alone: 6 in the capture after a cold-boot refusal,
0 after the candidate applied 24 CU.

Postflight therefore reports those values in `parameters_kmd_written`, captured and live, and never fails on
them. Every other captured value must still be equal, absence included, and `UnconfirmedStarts` must still be 0.
Values that exist live but not in the capture are listed in `parameters_added`. Attempts frozen before
2026-09-30 keep their own copy of the older comparison.

## Host tests

```
python tools\win\kmd-deploy\check-offline.py            44 checks, about 3 minutes
python tools\win\kmd-deploy\check-offline.py --quick    41 checks, without the bounded-child tests
python tools\win\kmd-deploy\tools\test_versions.py      the version rules, also part of both runs above
python tools\win\kmd-deploy\tools\test_accept.py <dry-run attempt dir>    11 checks on a scratch copy
```

`tools/quality/quick.ps1` runs `--quick` as the check `kmd-deploy`. The gate compiles every Python file, parses
all 87 PowerShell scripts with the Windows PowerShell 5.1 parser, proves that the kmd168 helpers in the template
are the 171 stage's own files byte for byte, and runs every transition host test. The bounded-child tests start
short-lived hidden `powershell.exe` children inside a job that the helper kills. Nothing resident and no window.

Two historical tests do not run: `test-select-driver-args.ps1` drives the device installer, and
`test-receipt-pipe.ps1` needs a fixture binary the 171 stage did not preserve.

`test_accept.py` needs a `stage.py freeze --dry-run --out` tree to work on:

```
python stage.py freeze --package <pkg> --rollback <pkg> --dry-run --out <BC250_ROOT>\scratch\kmd-deploy\host-tests\dryrun-X
python tools\test_accept.py <BC250_ROOT>\scratch\kmd-deploy\host-tests\dryrun-X
```

It checks that `accept` refuses an attempt with no receipts, one that was restored, one with no Cleanup, and a
failed or foreign postflight. It also checks that a print-only run writes nothing, that `--apply` moves only the KMD pins, and
that a second `--apply` refuses.

Negative controls of `freeze` itself, in `tools/test_versions.py` (part of `check-offline.py`):

- A version and an ABI that name different revisions, a build counter of 0 and a version that is not 0.7.R.B.
- A package INF and a source INF that name different `DriverVer` values, with no release rule to explain it.
- A release INF that differs from manifest.json, from its build or in more than the two release edits.
- A release SYS that is not its build SYS signed again, and a release package without `--rollback-build`.
- The same package as candidate and rollback, the same version with another SYS and the same SYS with another
  version.
- A rollback that is not the KMD or the release that `lab-baseline.json` names, and a wrong certificate.

The test also reads the tester.20 release over its build 0.7.216.14, and the 0.7.216.16 build as a candidate over
it, when those directories are in the workspace. `tools/test-identity-revisions.ps1` runs `test-identity.ps1` on
identities with build counters (216.16 over 216.100, 216.16 over 216.14, 217.1 over 216.100) and refuses a wrong
directory pattern, one version twice, build 0, a label without its build and a foreign ABI.

### The Get-FileHash trap, now closed

The kit used to carry an operator rule: start it from Git Bash or cmd, never from a PowerShell 7 parent, because
`Get-FileHash` then vanished in the 5.1 children and every staging test failed with
`CommandNotFoundException`. The cause is `PSModulePath`: a 5.1 child that inherits a PowerShell 7 parent's
value, or no value at all, loses the inbox modules. `kmdcommon.ps_env()` now spells the Windows PowerShell
module path out for every 5.1 child, so the kit runs from any shell, `quick.ps1` included. Nie ma rzeczy
niemozliwych, sa tylko zle ustawione zmienne srodowiskowe - there are no impossible things, only badly set
environment variables.

## Where the files go

| Path | What |
|---|---|
| this directory | The kit: the Python drivers, `ops/` for the lab-side runners, `template/` for the transition scripts, `tools/` for the host tests |
| `<BC250_ROOT>/scratch/kmd-deploy/attempts/` | One frozen directory per attempt, plus its `-ops` receipts directory. Never rewritten |
| `<BC250_ROOT>/scratch/kmd-deploy/host-tests/` | Host-test output, one UTC-stamped directory per run |
| `C:\BC250\m15\<attempt>` on unit A | The staged attempt |

`BC250_ROOT` is the workspace root, by default the parent directory of this repository.
`BC250_KMD_DEPLOY_WORK` moves the attempts and the host-test output elsewhere. Nothing is written to drive C: of
the development PC.

The kit also reads two directories of recorded history, by their hashes, and refuses to continue if either
changed: `<BC250_ROOT>/scratch/m14/kmd171-deploy001` holds the stage manifest of the measured 170 to 171
promotion and the `bounded-child.exe` and `select-driver.exe` that every transition reuses byte for byte.

## The operator copy

The copy at `<BC250_ROOT>\scratch\kmd-deploy` is the one the workspace rules and the deployment notes name, and
it holds the frozen attempts. This directory is the source of record. Change the file here first, then copy it
over after a deliberate check. The `attempts/` tree is not in this repository: it is tens of thousands of built
and signed driver packages, one set per candidate.

The per-version kits (`kmd173-deploy`, `cumode\kmd174-deploy`) stay in the workspace as frozen history.
