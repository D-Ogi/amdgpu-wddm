# GUI phase 1: the setup window and the install engine

Plan: GUI-PLAN-v7 (A1, A2, A3, C7, C17, C20, F-INST, G-EVT, G-STAGE, G-OFF, G-VER). This file fixes the formats
between the setup window (`tools/win/amdgpu_wddm_setup`) and the install engine (`tools/release/installer`). The
records that the control app reads are in `docs/gui/interfaces.md`: the engine writes the running-release witness
(section 1), the install-action fields of `state.json` (section 2) and the repair entry and recovery shortcut (section
4) exactly as defined there. Every reader treats a missing, damaged or unknown-schema record as "no record".

Rule for both sides: the window shows plain words chosen by ids (`message_id`, check `id`, `reason_id`). The
technical `detail` fields, KMD versions, fences and error codes go only to the log and the support report.

## 1. Invocation

The window starts `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
<package>\installer\install.ps1` hidden (CreateNoWindow), from its own elevated process, with:

| Argument | Meaning |
|---|---|
| `-Gui` | no prompt, no self-elevation, no restart; what is missing ends the run with a result that names it |
| `-InvocationId <guid>` | the window's id of this run; echoed in every event and in the result |
| `-EventsFile <path>` | events, appended (section 2) |
| `-ResultFile <path>` | the terminal result, written once (section 3); a stale file is deleted at the start |
| `-Plan` | checks and the decision only; changes nothing |
| `-AcceptTestSigning`, `-BitLocker HaveKey\|Suspend` | the user's consents, given only after the window asked |
| `-Repair`, `-FirmwareDir <dir>`, `-Verify` | as on the command line |
| `-DeadlineUtc <ISO 8601 UTC>` | stop at the first stop point after this time (section 10) |

`prepare-offline.ps1 -Destination <dir> [-FirmwareDir <dir>]` takes the same `-Gui`, `-InvocationId`, `-EventsFile`,
`-ResultFile` and `-DeadlineUtc`. The command-line installer (`install.cmd` without `-Gui`) keeps its behaviour, except that its
restart is now a planned one: it asks "Restart now?" and then calls `ExitWindowsEx(EWX_REBOOT)` with a planned
reason, never `Restart-Computer -Force` (C17). `uninstall.cmd` does the same.

Cancel: the window creates `<EventsFile>.cancel`. The engine looks for it only at a safe point (`cancel` events say
when one is open) and then ends with outcome `cancelled`. One mutating engine runs at a time: a real run holds
`%ProgramData%\amdgpu-wddm\installer\engine.lock` (exclusive handle, released by Windows when the process ends); a
second one ends with `refused` / `result.busy`. Dry runs and plans take no lock.

## 2. Events

One JSON object per line, UTF-8 without BOM, appended; the reader takes complete lines only. Common fields:
`schema` = `"amdgpu-wddm.engine-event/1"`, `invocation`, `seq` (1, 2, 3 ... per run), `utc`, `type`.

| `type` | Fields | When |
|---|---|---|
| `start` | `mode` (plan, dry-run, run, verify, prepare-offline), `gui`, `dry_run`, `package`, `contract`, `phase`, `deadline_utc`, `job` (`kill-on-close`, or `none: <reason>`) | first |
| `stage` | `id`, `text` | a stage begins: `preflight`, `test-signing`, `install`, `firmware`, `files`, `driver`, `settings`, `finish`, `verify`; prepare-offline: `prepare-check`, `firmware`, `copy`, `finish` |
| `check` | `id` (`<area>.<finding>`, for example `gpu.missing`, `firmware.folder-bad`), `result` (ok, warn, fail), `name`, `detail` | each preflight check |
| `decision` | `action` (install, upgrade, resume, repair, verify, already), `installed_version`, `package_version`, `phase`, `consents` (test-signing, bitlocker), `consents_given`, `restarts` (0, 1, 2), `firmware_source` (download, folder, package-folder), `firmware_dir`, `notes` (release-notes files in the package), `secure_boot`, `bitlocker`, `compatibility` (`ok`, `reasons`) | once, before any change |
| `settings-plan` | `summary` (`kept`, `updated`, `added`, `unchanged`, `command`), `rows` [`group`, `name`, `decision` (set, same, update, kept, command), `current`, `value`, `default`, `present`] | after the decision, before the first `step` (WU-006, WU-044) |
| `cancel` | `available`, `where` | a safe point opens or closes |
| `deadline` | `passed`, `where`, `deadline_utc`, `action` (`finishing`) | the deadline has passed inside a part that cannot stop; the run stops at the next stop point (section 10) |
| `test-child` | `pid` | host tests only (`AMDGPU_WDDM_TEST_CHILD_SECONDS` in a dry run or plan) |
| `install-action` | `action`, `boot_id` | the first real change of a run (state.json `mutation_*` saved) |
| `step` | `description`, `dry_run` | each change (in a dry run: each change that would be made) |
| `restart-required` | `reason_id` (restart.test-signing, restart.driver-package, restart.complete), `continuation` | a restart is needed |
| `witness` | `written`, `reason` | verify tried to write the running-release witness |
| `result` | `outcome`, `exit_code`, `message_id`, `mutated`, `stop` (`cancel`, `deadline` or null) | last |

## 3. Terminal result

`schema` = `"amdgpu-wddm.engine-result/1"`, written through a temporary file and a rename at the end of every run
that got past its start, also after a failed step. The window believes it only when `invocation` is the id it gave;
the exit code alone is never the outcome.

```json
{
  "schema": "amdgpu-wddm.engine-result/1",
  "invocation": "4f8f5a52-...",
  "engine": { "contract": "amdgpu-wddm.engine/1", "package_version": "0.7.199.100-tester.11" },
  "mode": "run", "dry_run": false,
  "started_utc": "...", "ended_utc": "...",
  "boot": { "boot_id": 41, "boot_utc": "..." },
  "action": "install",
  "outcome": "restart-required", "exit_code": 0,
  "mutated": true, "nothing_changed": false,
  "phase_before": null, "phase_after": "testsigning-pending",
  "restart": { "required": true, "reason_id": "restart.test-signing", "boot_id": 41,
               "continuation": { "kind": "continue", "exe": "...", "arguments": ["..."], "command": "...", "closure": "..." } },
  "consents_needed": [], "failed_checks": [],
  "message_id": "result.restart-test-signing", "step": null, "detail": "...", "log": "C:\\ProgramData\\...\\install-....log",
  "deadline_utc": null,
  "stop": null,
  "children": { "job": "kill-on-close", "left_at_exit": 0, "ended": 0 }
}
```

| `outcome` | `exit_code` | `message_id` |
|---|---|---|
| planned | 0 | result.planned |
| completed | 0 | result.dry-run-complete, result.dry-run-verify |
| already | 0 | result.already |
| restart-required | 0 | result.restart-test-signing, result.restart-driver, result.installed-restart |
| restart-required | 5 | result.testsigning-not-active, result.testsigning-secureboot |
| restart-required | 7 | result.restart-still-pending, result.verify-before-restart |
| verified | 0 | result.verified, result.verified-with-warnings |
| refused | 2 | result.preflight-refused (`failed_checks`), result.preflight-error, result.needs-admin, result.package-damaged, result.firmware-folder-bad, result.firmware-unreachable, result.prepare-destination |
| refused | 9 | result.busy |
| needs-consent | 4 | result.needs-consent (`consents_needed`) |
| verify-failed | 3 | result.verify-failed |
| failed | 5, 6 | result.testsigning-not-active, result.step-failed (`step` names it) |
| cancelled | 8 | result.cancelled, result.cancelled-after-changes (`stop.by` cancel), result.deadline, result.deadline-after-changes (`stop.by` deadline) |
| prepared | 0 | result.offline-prepared |

The engine never restarts Windows in a `-Gui` run. The window offers "Restart now" or "Later"; "Restart now" calls
`ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_FLAG_PLANNED | MAJOR_APPLICATION | MINOR_RECONFIG)`, never `EWX_FORCE`. A
boundary advances only in a new boot: the state keeps `restart_boot_id`, and a continuation that runs in the same boot
ends with `restart-required` / exit 7 and changes nothing.

## 4. Continuation closure and kept repair set (A1, WU-051, WU-058)

Before the first restart that an install asks for, every file that `manifest.json` lists, `manifest.json` itself
and, for an offline install, the firmware files are copied to
`%ProgramData%\amdgpu-wddm\installer\packages\<version>` and each SHA256 is checked there. RunOnce
(`HKLM\...\RunOnce\amdgpu-wddm-installer`) then names that copy: `"<closure>\setup\amdgpu_wddm_setup.exe" --continue`
for a run of the window (when the package has the window), else `"%windir%\System32\cmd.exe" /d /c
<closure>\install.cmd`; after phase 2, `"...\cmd.exe" /d /c "<install root>\verify.cmd"`. The program and each
argument are quoted on their own.

When phase 2 completes, the closure becomes the active repair set, completed with the installed firmware; the set
that was active before stays as the previous one, older sets are removed. Index:
`packages\index.json` = `{ "schema": "amdgpu-wddm.repair-sets/1", "updated_utc", "active": { "version", "dir",
"manifest_sha256", "firmware_complete", "setup" }, "previous": { ... } | null }`. `Release\RepairSetup` names the
active set's setup window. A rollback to the previous set is admitted only from Ph 3 (G-RB), by its compatibility
record (section 6), never by its version.

## 5. Prepared offline folder

`prepare-offline.ps1` (window: "Prepare an installation without internet") writes `<destination>.partial`, checks
everything there and renames it to `<destination>`. Layout: the package files as `manifest.json` lists them,
`firmware\` with the files of `manifest.json` `firmware`, and `offline-set.json` =
`{ "schema": "amdgpu-wddm.offline-set/1", "version", "name", "manifest_sha256", "prepared_utc", "firmware": {
"commit", "dir": "firmware", "source": "download" | "folder", "files": [ { "name", "sha256" } ] } }`. `install.cmd`
started from such a folder (or from a kept repair set) takes the firmware from its own `firmware\` folder and needs
no network; a missing or changed firmware file is refused by the preflight before any change. An existing prepared
folder is replaced as a whole; another non-empty folder is refused.

## 6. Compatibility record (C7, used by G-RB from Ph 3)

`compatibility.json` at the package root, listed in `manifest.json`, written by `build-release.ps1` and verified by
`installer\compatibility.ps1` (`Test-CompatibilityRecord`):

```json
{
  "schema": "amdgpu-wddm.compatibility/1",
  "version": "0.7.199.100-tester.11",
  "engine": { "contract": "amdgpu-wddm.engine/1", "result_schema": "amdgpu-wddm.engine-result/1",
              "event_schema": "amdgpu-wddm.engine-event/1", "witness_schema": 1 },
  "no_live_rebind": { "inf": "payload/kmd/bc250kmd.inf", "inf_sha256": "...", "reboot_directive": true,
                      "install_sections": ["Bc250_Install"] },
  "firmware": { "commit": "...", "install_dir": "C:\\BC250\\firmware", "files": [ { "name": "...", "sha256": "..." } ] },
  "settings": { "table": "installer/registry-defaults.json", "table_schema": 1, "table_sha256": "...",
                "groups": ["parameters", "desktop_router", "app_router", "d3d12_applications"] },
  "kmd": { "build": "0.7.199.1", "abi": "0x000700C7", "driver_ver": "0.7.199.100", "sys_sha256": "..." },
  "continuation": { "closure": true, "setup_exe": "setup/amdgpu_wddm_setup.exe" }
}
```

Refusal ids: `compat.package-damaged`, `compat.missing` (a package built before the record, tester.10 and
tester.11 included), `compat.unreadable`, `compat.unknown-schema`, `compat.other-package`, `compat.engine`,
`compat.live-rebind` (no Reboot directive in an install section of the INF, or the INF is not the recorded one),
`compat.firmware`, `compat.firmware-incomplete` (with a set's firmware folder), `compat.settings`, `compat.kmd`.

## 7. Release notes (WU-044, C16)

`RELEASE-NOTES.md` at the package root, English, from `docs/testing/release-notes/<version>.md`, with the sections
`## New`, `## Fixed`, `## Known issues`, `## Settings affected` (the build refuses notes without them). A
translation `RELEASE-NOTES.<lang>.md` is used when present; otherwise the window shows the English text with an
"in English" label.

## 8. Setup window

`<package>\setup\amdgpu_wddm_setup.exe` (WinForms, .NET Framework 4.8, `asInvoker`; it relaunches itself elevated
with `runas` when it has to change the system). Arguments: none (welcome), `--continue` (from RunOnce),
`--repair` (from the control app's Help, section 4 of interfaces.md), `--prepare-offline` (no elevation; it only
copies), and the options `--package <dir>` (another package than the exe's own), `--run-root <dir>` (where each
engine run keeps its events, result and log; default `%TEMP%\amdgpu-wddm-setup`, one `<utc>-<id8>` folder per run,
Administrators and SYSTEM only when elevated) and `--dry-run` (welcome or `--repair` only: the install run passes
`-DryRun` to the engine, so every screen is reached and nothing changes; lab item L4). `--version` prints the
version. Two headless test entries never show a window:

- `--smoke-render <dir> <scale> <lang> [--text-scale <f>] [--package <dir>]`: every screen from recorded engine
  output, drawn to `<dir>\<screen>.png` with its visible text, and `<dir>\layout.txt` starting with `ok: ` or listing
  overlaps, controls outside the window or the scroll range, G-A11Y and G-NOINT findings and missing strings;
- `--smoke-engine <package> <out-dir> [--script <name>] [--cancel] [--plan] -- <engine args>`: one real engine run
  through the same client as the window, `<out-dir>\summary.txt` (model, result binding, chosen screen, the screen
  in each language checked for internals) and `<out-dir>\support.zip`.

Strings: `strings.<lang>.txt` (`en`, `pl`, `ja`, `ko`) embedded in the exe, the control app's format
(`id|status|hash8|text`, status `mt`, `mt-safe` or `rev`, hash of the English text a line translates; English is
the source; `stamp.ps1` refreshes the hashes). The window's language is the control app's choice
(`HKCU\Software\amdgpu-wddm\Control`, read only), else the Windows display language, else English; the header
switches it. The tip panel follows the control app's guide setting: no character art in the setup window yet, the
same tip text with a plain title.

## 9. Running-release witness and verify report: how each value is obtained (writer side)

The format and the reader's rules are `docs/gui/interfaces.md` sections 1 and 2. The writer
(`installer\release-witness.ps1`, used by verify and by the start-confirm task) writes every field, never an empty
one; when a value cannot be obtained, it writes no witness and logs why.

| Field | Source |
|---|---|
| `boot_id` | `Get-BootIdentity` (release-witness.ps1), the one definition for the engine, verify, the task and the witness: `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters\BootId` read as an unsigned 32-bit number |
| `recorded_utc` | the writer's clock at the reading, ISO 8601 UTC |
| `recorded_by` | `verify` or `start-confirm` |
| `release`, `version` | `name` and `version` of `<install root>\manifest.json` |
| `manifest_sha256` | SHA256 of exactly that file, `<install root>\manifest.json` |
| `kmd_image_sha256` | SHA256 of the file at the path of the `bc250kmd.sys` module that Windows lists as loaded (`EnumDeviceDrivers`, `GetDeviceDriverFileName`); must equal the manifest's `payload/kmd/bc250kmd.sys` hash |
| `kmd_build` | the manifest's `kmd_build`, four parts (`0.7.199.1`); another shape gives no witness |
| `kmd_abi` | the manifest's `kmd_abi` (`0x` and 8 hex digits); must equal the driver's reply (`bc250kmd_cli info`) |

The loaded-image evidence holds only while the file at the loaded path is the image of this boot. So there is no
witness when that file was created or written after the boot started (`LastBootUpTime`), or when `state.json`
records an install action in this boot (`mutation_boot_id` = this boot): the start after the next restart writes it.
The file gets Administrators as its owner (SYSTEM when the task wrote it); a file whose owner cannot be set is
removed again.

Verify report: every verify run that reaches its checks writes
`%ProgramData%\amdgpu-wddm\installer\verify\verify-<yyyyMMddTHHmmssfffZ>.json` through a temporary file, also when the
GPU is missing or a check throws (then with a failing `verify` result):

```json
{
  "schema": "amdgpu-wddm.verify-report/1",
  "utc": "2026-10-04T12:00:00.000Z", "invocation": "...", "boot_id": 1234, "dry_run": false,
  "release": "amdgpu-wddm-tester-0.7.199.100-tester.11", "package_version": "0.7.199.100-tester.11",
  "manifest_sha256": "64 hex digits of <install root>\\manifest.json", "manifest_source": "install-root",
  "kmd_abi": "0x000700C7", "install_root": "C:\\Program Files\\amdgpu-wddm",
  "outcome": "passed", "complete": true, "passed": 9, "failed": 0,
  "results": [ { "check": "driver bound", "pass": true, "detail": "..." } ]
}
```

`outcome` is `failed` when any result fails, the list is empty or the run stopped before its last check
(`complete` false). `results[]` keeps the existing fields (`check`, `pass`, `detail`, and `warning` or `info` on
results that do not fail). `manifest_source` is `package` only when the install root has no manifest (a verify of an
uninstalled package); such a report does not bind an installed release.

## 10. Cancel, deadline and child processes (lab kits and the setup window)

A caller (the setup window, or a lab kit through its scenario adapter) runs the engine with `-Gui` and stops it in
one of three ways; only the first two end with a terminal result.

1. Cancel: create `<EventsFile>.cancel`.
2. Deadline: pass `-DeadlineUtc <ISO 8601 UTC>` (absolute, so the caller's clock and the engine's start time do not
   matter). An unreadable value counts as passed: the run stops at its first stop point, before any change.
3. Terminate the engine process (`powershell.exe`, the process the caller started). This is the hard bound.

Stop points, where the engine acts on 1 and 2 (the cancel file is read only there, the deadline only there):

| Run | Stop points | Parts that do not stop (finished first) |
|---|---|---|
| plan, dry run, verify (`install.ps1 -Plan`, `-DryRun`, `-Verify`) | every stage boundary, plus `before-changes`, `after-staging`, `before-driver-install` | none; verify's wait for the start-confirm task (up to 120 s) also ends at the deadline |
| install, repair, upgrade (real run) | `before-changes` (after preflight and the decision), `after-staging` (closure staged, before test signing), `before-driver-install` (phase 1 done, before files, driver package and settings) | phase 1 from `after-staging` to its restart (restore point, BitLocker, test signing); phase 2 from `before-driver-install` to the end (files, `pnputil`, settings, task, finish) |
| prepare-offline | every stage boundary before `finish`, `before-copy`, `after-copy` (each removes `<destination>.partial`) | `finish` (the rename) |

At a stop point after the deadline the engine ends with outcome `cancelled`, exit code 8, `message_id`
`result.deadline` (nothing changed) or `result.deadline-after-changes`, and `stop` =
`{ "by": "deadline", "where": "<stop point>", "deadline_utc": "..." }` (a cancel: `"by": "cancel"`,
`result.cancelled` / `result.cancelled-after-changes`). When the deadline passes inside a part that does not stop, the
engine logs it and writes one `deadline` event per stage (`action` `finishing`), finishes that part and stops at the
next stop point; a real install's phase therefore ends either completed or at its restart. A caller that cannot wait
for that uses 3.

Child processes: a `-Gui` run creates a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` and assigns itself to it
before it starts any process (`start` event `job`). Every process it starts afterwards (pnputil, bcdedit, the
probes, `powershell.exe` children, their own children) is in the job and cannot break away. The engine holds the only
handle, not inheritable: when the engine process ends for any reason, also when the caller terminates it, Windows
closes the handle and ends every process still in the job. At a normal end `Exit-Engine` ends any child still running
(only a process that is still in the job at that moment, so a reused process id is never touched; the installer
scripts contain no `Stop-Process`, BD-060) and records `children` = `{ "job": "kill-on-close", "left_at_exit": n, "ended": n }` in the result. Work that a
Windows service does for the engine (the PnP service's driver installation in `drvinst.exe`, WMI providers) is not
the engine's child and is not in the job; Windows bounds it itself. When the job cannot be created, `job` is
`none: <reason>` and only the caller's own job (if any) bounds the children.

After 3 there is no terminal result: the caller treats the run as "changes unknown" (A3). The installer state shows
how far the run got, and running the same package again continues or repairs it (phase 2 is idempotent).

For a 180 s lab bound: start the engine with `-DeadlineUtc` = launch + 120 s or less, leaving room for the part in
progress, and terminate the engine at 180 s if it has not exited; then check that no process remains whose parent
was the engine. Durations of the parts that do not stop are not measured yet on unit A (lab items L4, L5).

Tests (`tools/release/test-engine-events.ps1`, G-STAGE): a passed deadline stops a dry run at `stage:preflight` with
`result.deadline` and no step; an unreadable deadline the same; a later deadline lets a plan complete and is named in
`start` and the result; prepare-offline after its deadline writes no folder; at a normal end the engine ends its
left-over test child and grandchild (`cmd.exe`, `ping.exe`) and records them; an engine terminated by its caller
takes its child and grandchild with it.
