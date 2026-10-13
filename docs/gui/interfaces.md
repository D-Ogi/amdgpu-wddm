# GUI phase 1: files and values shared between the control app, setup/engine and the UMDs

Plan: GUI-PLAN-v7 (F-VER, A4, WU-043, WU-058, WU-068, C14) and its decisions of 497. Where the plan leaves a format
open, this file fixes a minimal versioned one. The control app (stream A, `tools/win/amdgpu_wddm_control`) only READS
the records of sections 1, 2 and 4 and writes only what sections 3 and 5 say it writes. Every reader treats a
missing, damaged or unknown-schema record as "no record": it never invents a success, a version or a launch from one.

## 0. The boot

`boot_id` is defined once, by the engine (`tools/release/installer/release-witness.ps1`, `Get-BootIdentity`):
`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters\BootId`, the REG_DWORD
that Windows increments at every boot, read as an unsigned 32-bit number. The app reads the same value the same way
and derives nothing else from it. An unreadable BootId means "the boot is unknown": no witness and no install action
is then of the current boot.

## 1. Running-release witness (written by the engine's verify step or the start-confirm task, stream B)

`%ProgramData%\amdgpu-wddm\installer\running-release.json`, owned by Administrators or SYSTEM (the app ignores a file
with another owner), written atomically (temporary file, then replace).

```json
{
  "schema": 1,
  "boot_id": 1234,
  "recorded_utc": "2026-10-04T12:00:00.000Z",
  "recorded_by": "verify",
  "release": "amdgpu-wddm-tester-0.7.199.100-tester.11",
  "version": "0.7.199.100-tester.11",
  "manifest_sha256": "64 hex digits of the package's manifest.json",
  "kmd_image_sha256": "64 hex digits of the driver image the started device loaded",
  "kmd_build": "0.7.199.1",
  "kmd_abi": "0x000700C7"
}
```

- `recorded_by`: `verify` or `start-confirm`. `release` is the manifest's `name`, `version` its `version`,
  `manifest_sha256` the SHA256 of `<InstallDir>\manifest.json`, `kmd_build` the manifest's four-part `kmd_build`,
  `kmd_abi` the manifest's `kmd_abi` (`0x` and 8 hex digits). Extra fields (`boot_utc`, `kmd_image_path`,
  `reply_version`, `driver_ver`) are for the support report only.
- The writer writes the record only when the loaded `bc250kmd.sys` image has the SHA256 that the manifest records
  for `payload/kmd/bc250kmd.sys`, the file was not created or written after the boot started, the driver's reply
  (BC250_KMD_VERSION) equals `kmd_abi`, and no install action ran in this boot. One package per record.

Two kinds of evidence stay apart: the driver's REPLY carries only the KMD ABI version (`kmd_abi`), never a build or a
release; the BUILD and the release come only from the loaded image and the manifests. The app never turns a reply
into a build or a release.

Reader acceptance rules. The app names a release as running ("exact") only when every rule holds; otherwise it says
"unknown" or, by the rule at the end, "one of several":

1. The file exists, has a trusted owner, parses as JSON, has `schema` 1 and every field above, non-empty and well
   formed: `boot_id` an integer 0..4294967295, `recorded_utc` an ISO 8601 time, `recorded_by` `verify` or
   `start-confirm`, `manifest_sha256` and `kmd_image_sha256` 64 hex digits (compared without case), `kmd_build` four
   dotted numbers, `kmd_abi` `0x` and 8 hex digits. A partial witness is no witness.
2. `boot_id` equals the current BootId.
3. The driver replies, and `kmd_abi` equals the reply's version (as numbers).
4. Every install action of this boot has a smaller elapsed boot stamp than the witness (section 2).
5. The manifest mapping is complete (every known package's manifest, section 2, is readable and has `name`,
   `version`, `kmd_build`, `kmd_abi` and the KMD component hash; no two manifests disagree about one release or one
   KMD image), and exactly one known package binds the whole witness: its manifest file's SHA256 equals
   `manifest_sha256`, and its `name`, `version`, `kmd_build`, `kmd_abi` and KMD component hash equal `release`,
   `version`, `kmd_build`, `kmd_abi` and `kmd_image_sha256`.
6. The loaded image. The witness's writers run elevated and record the loaded image's SHA256 in `kmd_image_sha256`
   together with this boot's `boot_id`; with rule 2 that record is the loaded-image evidence of this boot. The app
   runs as the invoking user (it is never elevated for this) and uses its own list of loaded drivers
   (`EnumDeviceDrivers`) only as a cross-check:
   - When Windows gives the app the image bases and `bc250kmd.sys` is among the listed modules, that module must be
     a file not created or written after the boot started, its SHA256 must equal `kmd_image_sha256`, and its build
     must equal `kmd_build`. Any difference fails the rule. The image carries no version resource; its build is the
     `kmd_build` that the known manifests record for that image hash (none, or manifests that disagree: no build).
   - When the bases are visible and `bc250kmd.sys` is not listed, or the list cannot be read, the rule fails.
   - When Windows returns null image bases (Windows 11 24H2 and later, for a process without SeDebugPrivilege;
     [EnumDeviceDrivers](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumdevicedrivers)), or
     a base has no name, and `bc250kmd.sys` is not among the named modules, the app has no evidence of its own. That
     is not a contradiction: the witness's record stands, and the support report says that the cross-check did not
     run.

When a rule fails, the app falls back: the mapping is complete and two or more known packages have the reply's
`kmd_abi` -> "one of several" (ambiguous). Every other case -> "unknown" (no reply, an incomplete or conflicting
mapping, one or no candidate without a valid witness).

The app's rule is a superset of the engine's reader rule (`Test-RunningReleaseWitness`: schema, writer, boot, ABI,
later install action); rules 1, 5 and 6 are the app's own additions.

## 2. Install actions, installed release and verification (engine, stream B)

Install actions: fields of `%ProgramData%\amdgpu-wddm\installer\state.json` (schema unchanged, extra fields):

- `mutation_boot_id` (number): the BootId of the boot of the newest mutating step (any step that changes Windows,
  the driver, files or settings; a dry run and verify are not mutations).
- `mutation_utc` (string, ISO 8601 UTC): the time that step started.
- `mutation_boot_elapsed_ms` (nonnegative integer): `GetTickCount64`, in milliseconds, at that step.

The witness also records `boot_elapsed_ms`. For an action in this boot, both stamps must exist and the action's stamp must be smaller.
Otherwise the app voids the witness. Equal stamps cannot establish the order. Legacy records cannot use UTC to preserve a witness after a mutation.
An action in another boot does not void the witness. No recorded mutation means no install action.

Installed release: `HKLM\SOFTWARE\amdgpu-wddm\Release\Version` (REG_SZ) only. When it is absent or not a string, the
installed release is unknown; the app never falls back to `state.json`'s `package_version` in the window (the
support report shows it, labelled as the installer state). The update check compares with the installed release
only.

Phase: `state.json` `phase` says only whether a restart is due or the install stopped: `installed` and
`driver-pending-restart` = installed, the installed release starts after the restart; `install-incomplete` = the
install stopped. The phase never says whether the package was verified.

Verification: the newest `%ProgramData%\amdgpu-wddm\installer\verify\verify-*.json` (format: the engine's
`Write-VerifyReport`) that is valid and belongs to the installed package. Valid: schema
`amdgpu-wddm.verify-report/1`, `dry_run` false, a non-empty `results` list whose entries have a boolean `pass`,
`outcome` `passed` or `failed`, and `passed` only with `complete` true and every result passing; `manifest_sha256`
is 64 uppercase hex digits, and `manifest_source` is not `package` (a verify of a package that is not installed never
binds). Belongs to the installed package: `package_version` equals `Release\Version` and `manifest_sha256` equals the
SHA256 of `<InstallDir>\manifest.json` (uppercase hex, ordinal comparison). Then `passed` -> "verified", `failed` -> "not verified". No such report (none, only
invalid ones, or only reports of another package) -> "could not be checked".

Decision 8, BD-138: select the greatest valid `boot_id` first. A report without that field ranks below every report with it.
Within the selected boot, compare `boot_elapsed_ms`, then `utc`. If any candidate lacks a valid elapsed stamp, order that boot's candidates by UTC.
Use this fallback for the whole boot, so comparisons remain transitive. Without BootId, compare UTC only. Tied outcomes select the failed report.
Elapsed stamps are nonnegative signed 64-bit integers. BootId is an unsigned 32-bit integer. Other values count as missing.

The search runs off the UI thread and reads every `verify-*.json` before choosing. File names do not establish order after a clock correction.
Invalid reports, other packages, files larger than 4 MiB and files with another owner are passed over.
An unreadable report or directory makes the search incomplete. The bounds are 2000 reports, 64 MiB and 10 s.
An incomplete search always means "could not be checked", even when a matching candidate was already read.

A failed report from an earlier boot becomes an attention line only when this boot has an exact witness of the installed version and manifest.
The line says that the last full check preceded a restart and the installed driver runs now. It does not claim a passed check.
A failure from this boot, a future boot or an unknown boot stays a problem. Without the exact witness, an earlier failure stays a problem too.
The upgrade success latch still requires a passed full check. It cannot use this attention state as verification.

Known packages for the "unknown / ambiguous" decision: `<InstallDir>\manifest.json` and every
`%ProgramData%\amdgpu-wddm\installer\packages\*\manifest.json` (the kept repair set and, from phase 3, the previous
package). A package is a candidate when its `kmd_abi` equals the reply's version. A manifest that does not exist is
no package. A manifest that exists but cannot be read (denied, another owner, larger than 4 MiB), or a `packages`
directory that exists but cannot be listed, makes the mapping incomplete (rule 5 of section 1 then fails).

A `state.json` that exists but cannot be read or parsed counts as an install action of this boot at an unknown
time (rule 4 of section 1 then fails); a missing `state.json`, or one with another owner (ignored), records none.

## 3. Recent launches (written by the D3D12 and D3D11 shells, stream C)

The contract is `docs/design/recent-launches.md` (format version 1, stream C; branch `gui/p1-recent-launch` until it
is merged). The app follows its reader, Clear and switch rules; in short:

- List `%LOCALAPPDATA%\amdgpu-wddm\recent-launches.txt`, read with `FileShare.ReadWrite | FileShare.Delete`, at most
  8 MiB, parsed strictly; anything invalid shows no entries (never a part); another format version shows no
  entries; a missing file is an empty list. The app never writes the list and takes no lock to read it.
- Up to 64 entries in the file's order (most recently launched first). `apis` bit 1 set -> "D3D12", otherwise
  "D3D11". Paths compare with `OrdinalIgnoreCase`; the file name comes from the path. Labels say "launched", never
  "played". D3D11 applications on the default CPU path are not recorded in version 1; the data page says so.
- Clear: when the user also turns the list off, write `RecordRecentLaunches` = 0 first; take the lock (byte 0 of
  `recent-launches.lock`, `FileStream.Lock(0, 1)` retried every 20 ms for up to 2 s, off the UI thread); delete
  `recent-launches.txt` and `recent-launches.tmp`; unlock. Never delete the lock file. No lock in 2 s: "could not be
  cleared now".
- Switch `HKCU\Software\amdgpu-wddm\Control\RecordRecentLaunches`: section 5.

## 4. Repair entry and the recovery shortcut (setup, stream B)

- `HKLM\SOFTWARE\amdgpu-wddm\Release\RepairSetup` (REG_SZ): the full path of the setup exe in the kept repair set.
  The app's Help -> Repair starts `"<RepairSetup>" --repair` as the invoking user (setup elevates itself). Absent or
  a missing file: Help says repair is not available on this PC and names the release package instead.
- Start-menu shortcut "Nagi Control App (recovery)" -> `amdgpu_wddm_control.exe --recovery`. The recovery view
  never loads `bc250control.dll`.

Shortcuts of the application (one writer, `Get-ControlShortcutPaths` in `installer\common.ps1`):

| Place | Name | Arguments | Left out by |
|---|---|---|---|
| Start menu (all users) | `Nagi Control App.lnk` | - | `-NoStartMenuShortcut` |
| Start menu (all users) | `Nagi Control App (recovery).lnk` | `--recovery` | `-NoStartMenuShortcut` |
| Desktop (all users) | `Nagi Control App.lnk` | - | `-NoDesktopShortcut` |

The names carry the product name Nagi (owner, 2026-10-13). Earlier releases wrote the same three shortcuts as
`amdgpu-wddm Control.lnk` and `amdgpu-wddm Control (recovery).lnk`. The same list holds those three old names as
legacy rows. An install over an older release and the uninstall remove an old name only when its target is our
control application (`Get-LegacyShortcutCleanup`). A file of that name that points elsewhere stays. The footprint of the uninstall counts an old name by the same rule. The shortcuts of a component
(`components.ps1`, `Get-ComponentShortcutPaths`) follow the same rule: `Nagi <title>.lnk` and
`Nagi <title> server.lnk`, and the old `amdgpu-wddm <title>` names go when they point into the component's folder.

**The Nagi wallpaper** (owner, 2026-10-13). The installer writes `HKLM\SOFTWARE\amdgpu-wddm\Release\Wallpaper`
(REG_DWORD: 1, or 0 after the box of the setup window was cleared or `-NoWallpaper`) and `Release\WallpaperStamp`
(REG_SZ, `<version> <UTC>`, new at every install run). It sets no picture itself. The application sets the picture
for the person who signs in when `Wallpaper` is 1, the stamp is not this account's `WallpaperApplied` (section 5),
and the installer's `state.json` says `verified` for the installed `Release\Version`. Two starts that exist already
reach this rule. The first is the sign-in notice (`--start-notice`), which the logon task "amdgpu-wddm start window"
starts for every person, with that person's rights. It waits up to 300 s for the verification of that logon. The
second is the first read of the window. The picture comes from `<InstallDir>\control\nagi-wallpaper.nwp`
(`tools/win/amdgpu_wddm_control/art/wallpaper/PROVENANCE.md`). The application decodes it into
`%LOCALAPPDATA%\amdgpu-wddm\wallpaper\nagi-wallpaper.jpg` and sets it with the position Fill. Before that it saves
the background of the account (`WallpaperPrevious`). Settings, "Nagi wallpaper", sets the picture again or restores
the saved background. A slideshow or Windows Spotlight comes back as its last picture only, and the card says so. A
failure is one line in `wallpaper.log` in the same folder and changes nothing else. The uninstall changes no
background and keeps `%LOCALAPPDATA%\amdgpu-wddm\wallpaper`.

Both places are written by default. Each shortcut takes its icon from the application (`IconLocation` =
`<exe>,0`) and its working directory is the install folder. The setup window offers the two places as checked
boxes and states both answers on the command line, so a run after a restart can put back a shortcut the first run
left out (`-StartMenuShortcut`, `-DesktopShortcut`). The uninstall removes all three.

## 5. Control app preferences (HKCU, app state only; the installer never writes them)

`HKCU\Software\amdgpu-wddm\Control` (all REG_DWORD unless noted). Rule R4: the default is "value absent".

| Value | Absent means | Written |
|---|---|---|
| `Language` (REG_SZ `en`/`pl`/`ja`/`ko`) | Windows UI language, else `en` | on a language choice |
| `UpdateCheckAtStart` | on (D2) | 0 when unchecked; removed when checked |
| `RecordRecentLaunches` | on (D3) | 1 when checked, 0 when unchecked (read by the shells, section 3). Any value other than DWORD 0 or 1 records nothing; the app shows it as off and "not valid" until the user sets it |
| `ShowNagi` | on: the guide character's art ships in the application, so a fresh install shows her. The setup window reads this value by the same rule | 0 when unchecked; removed when checked |
| `ShowTipsAutomatically` | off | 1 when checked; removed when unchecked |
| `ReduceAnimations` | off (Windows "Animation effects" off also reduces) | 1 when checked; removed when unchecked |
| `ShowSupportOptions` | off (D5) | 1 when checked; removed when unchecked |
| `GettingStartedDismissed` | the card shows | 1 on "Dismiss"; removed by Help -> "Getting started" |
| `HiddenGames` (REG_MULTI_SZ) | nothing hidden | the file names (`witcher3.exe`) of the hidden entries; the Games list has one entry per file name, as the profiles do (B3) |
| `ShownGames` (REG_MULTI_SZ) | every recorded launch that is not a game stays hidden. | The file names of the entries that the person chose to see among the games with "Show". These entries are not games. They are web browsers and web views, parts of Windows, game store clients and tools of this driver (`LaunchKinds` in `RecentLaunches.cs`, BD-140) |
| `LastSeenRelease` (REG_SZ) | - | the release the app last showed as "upgrade done", for the one-time verdict |
| `NameTipRelease` (REG_SZ) | the name tip is due | the installed release that the guide panel showed the name tip for. The tip says what the name Nagi stands for. It shows for 15 s at the first start of each installed release |
| `WallpaperApplied` (REG_SZ) | the Nagi wallpaper is due when the installer chose it | the `Release\WallpaperStamp` this account had the Nagi wallpaper set for, so it is set once per install (section 4) |
| `WallpaperPrevious` (REG_SZ) | nothing to restore | the background this account had before the Nagi wallpaper. Five fields separated by vertical bars: kind (picture, solid, slideshow, Spotlight), picture path, `WallpaperStyle`, `TileWallpaper` and colour |
| `BenchmarkFolder` (REG_SZ) | the ray tracing benchmark looks for the Steam copy of the game itself (section 7) | the folder the person chose, written only when that folder held a usable copy. This release has no control that clears it again |

Update check cache: `HKCU\Software\amdgpu-wddm\Control\Update` (REG_SZ values; times ISO 8601 UTC):

| Value | Meaning |
|---|---|
| `LastAttemptUtc`, `LastAttemptOutcome` | the newest attempt and its outcome (`Available`, `UpToDate`, `NewestOnly`, `Incomplete`, `Failed`, `RateLimited`) |
| `LastSuccessUtc` | the newest attempt that read the release list |
| `CheckedInstalled` | `Release\Version` as it was when that successful check compared against it |
| `CandidateTag`, `CandidateVersion`, `CandidateUrl`, `CandidatePublishedUtc` | the newest release the successful check saw, newer than the installed one or not |
| `InstalledTag`, `InstalledPublishedUtc` | the installed release as that check saw it in the list (source of the Driver card's "Released" date) |
| `NextAllowedUtc` | the earliest next automatic attempt (rate limit) |

No result is stored as a plain "current" flag; the app decides on every read:

- "Up to date" and the "Released" date only while `Release\Version` is readable and equals `CheckedInstalled`
  (`InstalledTag` likewise for the date). A rollback, another install or an unreadable `Release\Version` voids
  both until the next successful check.
- "Update available" when the candidate is newer than the installed release read now (the installed release must
  be readable).

## 6. The waiting time for the graphics (installer and control app share one value)

`HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers` `TdrDelay` (REG_DWORD, seconds) holds how long Windows
waits for the graphics before it resets them. It is Windows' own value. The Windows driver documentation gives it in
`display/tdr-registry-keys.md`. An absent value means 2 seconds. Windows reads the value when it starts. Every change
of it applies at the next restart of Windows. BD-079 and issue #12 are why this release owns the value. This part has
no working GPU reset, so a picture that needs more time than the wait ends as a stopped machine.

`install.ps1` writes the release's number. The number is `defaults.graphics_drivers.TdrDelay` of
`registry-defaults.json`, which is 10. The upgrade rule is the one of every other default. An absent value is set. A
value equal to what the previous installer wrote is updated. Any other value is kept. The value from before the first
write goes into the installer state as `tdr_delay`, with the fields `present`, `previous` and `wrote`.

`uninstall.ps1` writes the recorded value again. It removes the value when there was none before the installation. It
does either only while the current value is the one this release wrote.

The control app reads the value into the snapshot as `TdrDelay` and `TdrError`. It shows the value on the Help page,
under "Waiting time for the graphics", and in the recovery view. It writes a chosen number through the planned action
`tdr-delay` (`--action tdr-delay --tdr <seconds>`).

`manifest.json` carries the same table as `defaults.graphics_drivers`. The app's "Restore default graphics settings"
reads it.

`src/TdrSetting.cs` holds the rules of the app. It accepts 2 to 60 seconds. It offers the numbers 2, 5, 10, 20, 30
and 60. A stored number outside that list stays as it is until the person makes a choice. The chosen number is always
written. The app never removes the value. An absent value would get the release default again at the next
installation, against the release-notes rule "The installation keeps the settings that you changed". The app writes
no other value of that key.

## 7. Ray tracing benchmark results (control app only, per user)

The ray tracing benchmark card of the Performance page keeps what this computer measured, and nothing else keeps or
reads it. Both paths are below `%LOCALAPPDATA%\amdgpu-wddm\benchmarks` (the app's state directory, which
`AMDGPU_WDDM_CONTROL_STATE` moves for a build gate, so a build writes nothing into a profile):

| Path | What it holds |
|---|---|
| `q2rtx-history.json` | the newest ten runs, newest first |
| `q2rtx-home\` | the game's one writable directory while a pass runs (`+set homedir`), where it writes `baseq2\logs\q2rtx-bench-pipeline.log` and `q2rtx-bench-query.log` |

The file is UTF-8 JSON, `{"schema": 1, "runs": [...]}`. A run holds `utc` (ISO 8601 UTC, as every other time of this
app), `driver` (the installed release, empty when it could not be read), `width`, `height` and `passes`. A pass holds
`api` (`pipeline` or `query`), `outcome`, and the numbers it has: `fps`, `frames`, `seconds`, `max_temp_c`,
`mean_clock_mhz`. An outcome is one of `done`, `no-result` (the game closed before the scene ended), `timeout` (over
three minutes), `hot` (the thermal stop), `cancelled` and `failed` (the game did not start, or the pass refused to
start it). Only `done` carries `fps`.

The reader rule: a missing file, a file above 1 MB, a file that is not this JSON, another `schema`, and a run without
a readable time, without a picture size, without passes, with an unknown `api` or `outcome`, or marked `done` without
a number, are all "no history". A damaged run between sound ones is dropped and the sound ones are still shown. No
damaged file ever becomes a result and none of them stops the next run from being saved: a save writes a temporary
file next to the history and replaces it. A failed write loses the history and nothing else.

`src/Benchmark.cs` (`BenchHistory`, `BenchPaths`) holds the format and the rule. `test/BenchmarkTests.cs` decides
every case above.
