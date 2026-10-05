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
4. No install action of this boot is later than `recorded_utc` (section 2).
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

Install actions: two fields of `%ProgramData%\amdgpu-wddm\installer\state.json` (schema unchanged, extra fields):

- `mutation_boot_id` (number): the BootId of the boot of the newest mutating step (any step that changes Windows,
  the driver, files or settings; a dry run and verify are not mutations).
- `mutation_utc` (string, ISO 8601 UTC): the time that step started.

The app voids a witness when `mutation_boot_id` equals the current BootId and `mutation_utc` is later than the
witness's `recorded_utc` (or cannot be read). Absent fields mean "no install action recorded". An install action in
another boot does not void anything.

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

The search: off the window's UI thread, every `verify-*.json` in name order, newest first (the engine names them
`verify-<yyyyMMddTHHmmssfffZ>.json`), until the first valid report of the installed package. Invalid reports, reports
of another package, files larger than 4 MiB and files with another owner are passed over. A report that exists but
cannot be read, a `verify` directory that cannot be listed, or the search's bound (2000 reports, 64 MiB, 10 s) ends
the search as incomplete: "could not be checked" unless the report was found before.

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
- Start-menu shortcut "amdgpu-wddm Control (recovery)" -> `amdgpu_wddm_control.exe --recovery`. The recovery view
  never loads `bc250control.dll`.

## 5. Control app preferences (HKCU, app state only; the installer never writes them)

`HKCU\Software\amdgpu-wddm\Control` (all REG_DWORD unless noted). Rule R4: the default is "value absent".

| Value | Absent means | Written |
|---|---|---|
| `Language` (REG_SZ `en`/`pl`/`ja`/`ko`) | Windows UI language, else `en` | on a language choice |
| `UpdateCheckAtStart` | on (D2) | 0 when unchecked; removed when checked |
| `RecordRecentLaunches` | on (D3) | 1 when checked, 0 when unchecked (read by the shells, section 3). Any value other than DWORD 0 or 1 records nothing; the app shows it as off and "not valid" until the user sets it |
| `ShowNagi` | off | 1 when checked; removed when unchecked |
| `ShowTipsAutomatically` | off | 1 when checked; removed when unchecked |
| `ReduceAnimations` | off (Windows "Animation effects" off also reduces) | 1 when checked; removed when unchecked |
| `ShowSupportOptions` | off (D5) | 1 when checked; removed when unchecked |
| `GettingStartedDismissed` | the card shows | 1 on "Dismiss"; removed by Help -> "Getting started" |
| `HiddenGames` (REG_MULTI_SZ) | nothing hidden | the file names (`witcher3.exe`) of the hidden entries; the Games list has one entry per file name, as the profiles do (B3) |
| `LastSeenRelease` (REG_SZ) | - | the release the app last showed as "upgrade done", for the one-time verdict |

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
