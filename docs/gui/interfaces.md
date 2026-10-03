# GUI phase 1: files and values shared between the control app, setup/engine and the UMDs

Plan: GUI-PLAN-v7 (F-VER, A4, WU-043, WU-058, WU-068, C14). Where the plan leaves a format open, this file fixes a
minimal versioned one. The control app (stream A, `tools/win/amdgpu_wddm_control`) only READS the records of
sections 1, 2 and 4 and writes only what section 3 and 5 say it writes. Every reader treats a missing, damaged or
unknown-schema record as "no record": it never invents a success, a version or a launch from one.

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

- `boot_id`: `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters\BootId` of
  the boot the record was written in (the same value the app's BD-060 observer reads).
- `recorded_by`: `verify` or `start-confirm`.
- The writer writes the record only when the started device's image SHA256 equals the manifest's `kmd` component
  hash and the driver's reply (BC250_KMD_VERSION) equals `kmd_abi`. One package per record.
- The app accepts it as the exact running release only when: schema 1; `boot_id` equals the current BootId;
  `kmd_abi` parses (hex `0x...` or decimal) to the current driver reply's version; the first three parts of
  `kmd_build` equal that reply's `major.minor.revision`; and no install action is recorded after it in this boot
  (section 2).

## 2. Install actions in this boot (engine, stream B)

Two fields added to `%ProgramData%\amdgpu-wddm\installer\state.json` (schema unchanged, extra fields):

- `mutation_boot_id` (number): the BootId of the boot of the newest mutating step (any step that changes Windows,
  the driver, files or settings; a dry run and verify are not mutations).
- `mutation_utc` (string, ISO 8601 UTC): the time that step started.

The app voids a witness when `mutation_boot_id` equals the current BootId and `mutation_utc` is later than the
witness's `recorded_utc`. Absent fields mean "no install action recorded". The app also reads `phase` and
`package_version` (existing fields): `installed` and `driver-pending-restart` = installed, waiting for the restart;
`verified` / `verify-failed` = verification outcome; `install-incomplete` = install stopped. Verification details come
from the newest `%ProgramData%\amdgpu-wddm\installer\verify\verify-*.json` whose `package_version` equals the
installed version (existing format: `results[].pass`).

Known packages for the "unknown / ambiguous" decision: `<InstallDir>\manifest.json` and every
`%ProgramData%\amdgpu-wddm\installer\packages\*\manifest.json` (the kept repair set and, from phase 3, the previous
package). A package matches the reply when its `kmd_abi` equals the reply's version.

## 3. Recent launches (written by the UMD frontends, stream C; read, pruned and cleared by the app)

Per user, local only: `HKCU\Software\amdgpu-wddm\RecentLaunches` (key value `Schema` REG_DWORD 1).

One subkey per launched executable. Subkey name: 32 lower-case hex digits, the first 16 bytes of SHA-256 over the
UTF-16LE bytes of the normalized full path. Normalization: `GetFullPathNameW`, a `\\?\` or `\\?\UNC\` prefix removed
(UNC becomes `\\`), then `ToUpperInvariant` (simple upper-case mapping, as `CharUpperBuffW` with the invariant
locale; no Unicode normalization form).

Values of a subkey (all required; an entry with a missing or mismatching value is skipped by the reader):

| Value | Type | Meaning |
|---|---|---|
| `Path` | REG_SZ | the full path as `GetModuleFileNameW(NULL)` gave it (the subkey name is its hash) |
| `Image` | REG_SZ | the file name part of `Path` |
| `LastLaunchUtc` | REG_QWORD | FILETIME (UTC) of the newest recorded launch |
| `Api` | REG_SZ | `D3D12`, `D3D11` or `Vulkan`: the OUTER API of that launch |
| `Launches` | REG_DWORD | count of recorded launches, saturating |

Commit protocol (one bounded protocol for writers, prune and clear):

- Mutex `Local\amdgpu-wddm-recent-launches`. A UMD writer tries it with a 0 ms wait (`WaitForSingleObject(m, 0)`;
  `WAIT_ABANDONED` counts as taken); when busy it drops the entry. It never waits, never fails device creation.
- Under the mutex the writer reads the switch `HKCU\Software\amdgpu-wddm\Control\RecordRecentLaunches` (REG_DWORD):
  absent = ON (owner decision D3), 0 = OFF, any other value = ON; a read error other than "not found" = skip. Only
  then it creates or opens the subkey and writes the five values.
- Once per process, after a successful OUTER device creation only (D3D12 `CreateDevice` of the shell, the D3D11
  route's device, the ICD's `vkCreateDevice` only for an application instance, never for a hosted/internal instance
  of our own frontends). A process-wide named flag (for example an atom or a `Local\amdgpu-wddm-recent-launch-<pid>`
  event created first) keeps a nested D3D -> Vulkan creation to one record.
- Not recorded: an image under `%windir%` (directory-boundary compare: the path starts with `%windir%\`), our own
  tools (`amdgpu_wddm_*.exe`, `bc250*.exe`, `vulkaninfo*.exe`), session 0, a process in an AppContainer.
- The app's "Clear the list": writes `RecordRecentLaunches` = 0 only when the user also turns the switch off; then
  takes the mutex (2 s bound; when not taken it reports that the list could not be cleared now) and deletes every
  subkey. Prune: under the mutex, the 50 newest by `LastLaunchUtc` stay.
- Turning the switch back on removes the `RecordRecentLaunches` value (absent = on).

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
| `RecordRecentLaunches` | on (D3) | 0 when unchecked; removed when checked (read by the UMDs, section 3) |
| `ShowNagi` | off | 1 when checked; removed when unchecked |
| `ShowTipsAutomatically` | off | 1 when checked; removed when unchecked |
| `ReduceAnimations` | off (Windows "Animation effects" off also reduces) | 1 when checked; removed when unchecked |
| `ShowSupportOptions` | off (D5) | 1 when checked; removed when unchecked |
| `GettingStartedDismissed` | the card shows | 1 on "Dismiss"; removed by Help -> "Getting started" |
| `HiddenGames` (REG_MULTI_SZ) | nothing hidden | the hidden entries' keys |
| `LastSeenRelease` (REG_SZ) | - | the verified release the app last showed, for the one-time "upgrade done" verdict |

Update check cache: `HKCU\Software\amdgpu-wddm\Control\Update` (REG_SZ values, ISO 8601 UTC):
`LastAttemptUtc`, `LastAttemptOutcome`, `LastSuccessUtc`, `CandidateTag`, `CandidateVersion`, `CandidateUrl`,
`CandidatePublishedUtc`, `NextAllowedUtc`. No result is ever stored as a plain "current" flag.
