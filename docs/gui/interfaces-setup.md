# GUI phase 1: the setup window and the install engine

Plan: GUI-PLAN-v7 (A1, A2, A3, C7, C17, C20, F-INST, G-EVT, G-STAGE, G-OFF, G-VER). This file fixes the formats
between the setup window (`tools/win/amdgpu_wddm_setup`) and the install engine (`tools/release/installer`). The
records that the control app reads are in `docs/gui/interfaces.md`: the engine writes the running-release witness
(section 1), the install-action fields of `state.json` (section 2) and the repair entry and recovery shortcut (section
4) exactly as defined there. Every reader treats a missing, damaged or unknown-schema record as "no record".

Rule for both sides: the window shows plain words chosen by ids (`message_id`, check `id`, `reason_id`). The
technical `detail` fields, KMD versions, fences and error codes go only to the log and the support report.

## 1. Invocation

The window starts `powershell.exe -NoProfile -ExecutionPolicy Bypass -File <package>\installer\install.ps1` hidden
(CreateNoWindow), from its own elevated process, with:

| Argument | Meaning |
|---|---|
| `-Gui` | no prompt, no self-elevation, no restart; what is missing ends the run with a result that names it |
| `-InvocationId <guid>` | the window's id of this run; echoed in every event and in the result |
| `-EventsFile <path>` | events, appended (section 2) |
| `-ResultFile <path>` | the terminal result, written once (section 3); a stale file is deleted at the start |
| `-Plan` | checks and the decision only; changes nothing |
| `-AcceptTestSigning`, `-BitLocker HaveKey\|Suspend` | the user's consents, given only after the window asked |
| `-Repair`, `-FirmwareDir <dir>`, `-Verify` | as on the command line |

`prepare-offline.ps1 -Destination <dir> [-FirmwareDir <dir>]` takes the same `-Gui`, `-InvocationId`, `-EventsFile`
and `-ResultFile`. The command-line installer (`install.cmd` without `-Gui`) keeps its behaviour, except that its
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
| `start` | `mode` (plan, dry-run, run, verify, prepare-offline), `gui`, `dry_run`, `package`, `contract`, `phase` | first |
| `stage` | `id`, `text` | a stage begins: `preflight`, `test-signing`, `install`, `firmware`, `files`, `driver`, `settings`, `finish`, `verify`; prepare-offline: `prepare-check`, `firmware`, `copy`, `finish` |
| `check` | `id` (`<area>.<finding>`, for example `gpu.missing`, `firmware.folder-bad`), `result` (ok, warn, fail), `name`, `detail` | each preflight check |
| `decision` | `action` (install, upgrade, resume, repair, verify, already), `installed_version`, `package_version`, `phase`, `consents` (test-signing, bitlocker), `consents_given`, `restarts` (0, 1, 2), `firmware_source` (download, folder, package-folder), `firmware_dir`, `notes` (release-notes files in the package), `secure_boot`, `bitlocker`, `compatibility` (`ok`, `reasons`) | once, before any change |
| `settings-plan` | `summary` (`kept`, `updated`, `added`, `unchanged`, `command`), `rows` [`group`, `name`, `decision` (set, same, update, kept, command), `current`, `value`, `default`, `present`] | after the decision, before the first `step` (WU-006, WU-044) |
| `cancel` | `available`, `where` | a safe point opens or closes |
| `install-action` | `action`, `boot_id` | the first real change of a run (state.json `mutation_*` saved) |
| `step` | `description`, `dry_run` | each change (in a dry run: each change that would be made) |
| `restart-required` | `reason_id` (restart.test-signing, restart.driver-package, restart.complete), `continuation` | a restart is needed |
| `witness` | `written`, `reason` | verify tried to write the running-release witness |
| `result` | `outcome`, `exit_code`, `message_id`, `mutated` | last |

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
  "message_id": "result.restart-test-signing", "step": null, "detail": "...", "log": "C:\\ProgramData\\...\\install-....log"
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
| cancelled | 8 | result.cancelled, result.cancelled-after-changes |
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
`--repair` (from the control app's Help, section 4 of interfaces.md), `--package <dir>`, `--prepare-offline`;
`--smoke-render <dir> <scale> <lang>` and `--smoke-engine <package>` are headless test entries that never show a
window. Strings: `setup\strings\<lang>.json` (`en`, `pl`, `ja`, `ko`), each entry with `text`, `source_sha256` (of
the English text it translates) and `status` (`mt`, `mt-safe`, `rev`); English is the source.
