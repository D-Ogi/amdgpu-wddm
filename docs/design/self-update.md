# Self-update of Nagi, stage 1

This page describes how an installed release of Nagi installs the next release from inside the
control application. Stage 1 starts from the Driver page, with one UAC prompt. Stage 2 is a
scheduled task. This page names stage 2 at the end, but stage 2 is not part of this design.

## Parts

| Part | File | Task |
|---|---|---|
| Release writer | `tools/release/nagi_update.py` | Writes the content identities, the apps pack and the update manifest |
| Release signing | `tools/release/build-release.ps1`, `tools/release/user-mode-signing.ps1` | Signs the update manifest and checks the signature against the pinned list |
| Control application | `tools/win/amdgpu_wddm_control/src/SelfUpdate.cs` | Reads the manifest, checks it, downloads the assets and starts Setup |
| Setup window | `tools/win/amdgpu_wddm_setup/src/ApplyUpdate.cs`, `ApplyWindow.cs` | Runs `setup.exe --apply <dir>` as administrator and shows the outcome |
| Apply engine | `tools/release/installer/apply.ps1` | Checks the update again and installs the changed components |
| Installer library | `tools/release/installer/nagi-update.ps1` | Holds the plan, the apps switch, the transaction and the rollback |
| Start hooks | `start-confirm.ps1`, `install.ps1` | Record each start of a new driver and decide the transaction |

## Components

An update has three component kinds.

- `driver`: the kernel-mode driver, the user-mode drivers, the ICDs, the CLI and the task scripts.
  This kind is one unit. Its asset is the full release zip, `amdgpu-wddm-tester-<release>.zip`.
  The release's own `setup.exe` installs it on the normal install path. A driver update needs a
  restart.
- `apps`: the control application folder, `payload/control/`. Its asset is
  `nagi-apps-<release>.zip`. An apps update goes into a new versioned folder and switches a link.
  It needs no restart.
- `data`: a data pack, such as application profiles. A data pack goes into a new versioned folder
  and switches a pointer file. It needs no restart. Tester.37 ships no data pack, but the plan
  and the engine handle the kind.

The driver package also holds the apps. A plan with a driver change therefore never lists the
apps pack: the normal install of the package installs both.

## Content identity

Each component has a `content_id`. The writer computes it as the SHA-256 of the sorted lines
`path<TAB>digest` of the files of that component.

The digest of a PE file is its Authenticode digest. That digest does not include the signature,
so a new signature or a new time stamp is not a change. Every other file uses its SHA-256.

Some files change in each release by design. These files are not part of an identity: the INF
and the catalog, the certificate, the documents at the package root, the licence texts and
`compatibility.json`.

Each release stamps its version into every PE file. Thus two releases never have the same driver
identity, and each update today is a driver update. An apps-only update becomes possible when a
release keeps the driver files byte for byte. The plan and the engine are ready for that case now.

`build-release.ps1` writes the identities into the package `manifest.json` as
`update_components`. An installed release thus names the content that it installed.

## The update manifest

The release flavour ships three more assets on its GitHub release.

| Asset | Content |
|---|---|
| `nagi-update.json` | The update manifest, schema `nagi.update/1` |
| `nagi-update.json.p7s` | The detached signature of the manifest bytes |
| `nagi-apps-<release>.zip` | The apps pack |

The lab flavour gets no update assets.

The manifest has these fields.

- `schema`, `release` and `min_installed`. An install older than `min_installed` gets no update
  from inside the application. The default of `min_installed` is the release that the new release
  supersedes. `build-release.ps1 -UpdateMinInstalled` sets another value.
- `components`: one entry for each component, with `name`, `kind`, `version`, `asset`, `size`,
  `sha256` of the asset and `content_id`.
- The driver entry adds `kmd_abi`, `catalog_id`, `state_schema`, the hash of the KMD and the
  SHA-256 of the package `manifest.json`.
- The apps entry adds `kmd_abis`, `state_schemas` and the list of its files, each with a SHA-256.

## The signature

The signature is a detached CMS SignedData with SHA-256 over the exact manifest bytes. The signer
is the signer of every user-mode file of the release.

`build-release.ps1` runs signtool with `/p7`, `/p7co 1.2.840.113549.1.7.1` and
`/p7ce DetachedSignedData`. Signtool writes the bare SignedData structure. A CMS reader wants that
structure inside a ContentInfo. `ConvertTo-CmsContentInfo` adds that wrapper. The build then
checks the result against the shipped pinned list, and a failed check stops the build.

The signature has no time stamp. The installed release checks the signature and the pinned
signer itself. The validity period of the certificate does not apply.

The reader does not check the certificate chain. The test certificate is not a trusted root on
every computer. Also, a chain to a trusted root alone accepts any publisher. The pinned list holds
the SHA-1 thumbprints of the allowed signers. `SelfUpdate.cs` and `nagi-update.ps1` each hold the
list, and `test-nagi-update.ps1` makes sure that the two lists are the same.

## The control application

The Driver page shows "Install update" when the update check finds a newer release. The button
shows only when the installed release is known and the kept repair set is present.

The control application does these steps on a background thread.

1. Downloads `nagi-update.json` and `nagi-update.json.p7s` of the candidate tag.
2. Checks the signature and the pinned signer.
3. Reads the manifest and refuses a manifest that is not complete.
4. Makes the plan against the installed state.
5. Downloads only the assets of the plan into `%LOCALAPPDATA%\amdgpu-wddm\update\<release>`.
6. Checks the size and the SHA-256 of each asset.
7. Starts the `setup.exe` of the kept repair set with `--apply "<dir>" --wait-pid <pid>`, through
   `runas`.
8. Closes, so that Setup can replace its files.

Downloads use HTTPS only. A redirect must go to the default port of one of three hosts:
`github.com`, `objects.githubusercontent.com` and `release-assets.githubusercontent.com`.

The update check compares the candidate with the effective installed release. That release comes
from `installed.json` when the record names the installed driver identity. Otherwise it is the
release of the installed package.

## The plan

The plan is the same in `SelfUpdate.Plan` and in `Get-NagiApplyPlan`. Both answer the cases of
`tools/release/nagi-update-plan-cases.json`.

| Condition | Outcome |
|---|---|
| The installed release is not known | refused, `unknown-installed` |
| The manifest release is older than the installed release | refused, `downgrade` |
| The two releases are the same | up to date |
| The installed release is older than `min_installed` | refused, `too-old` |
| The new apps do not speak the KMD ABI or the state schema that runs after the update | refused, `incompatible` |
| The driver identity changed | apply the driver component, restart |
| Only the apps identity changed | apply the apps pack, no restart |
| A data pack identity changed | apply that data pack, no restart |
| Nothing changed | up to date |

The refusals show as plain text in the four languages. The text names no internal term.

## The setup window and the engine

`setup.exe --apply <dir>` asks for administrator rights when it does not have them. It waits for
the control application to end. Then it runs `installer\apply.ps1` of its own package in Windows
PowerShell 5.1. A small window shows while the engine runs. A message box shows the outcome.

The setup of the installed release runs the engine. Thus the installed release checks the next
release.

The engine does these steps.

1. Ends an apps switch that a stop interrupted.
2. Copies the manifest and the signature into the protected update folder and checks them there.
3. Makes the plan again against the installed state.
4. Copies each asset of the plan into the protected folder and checks its SHA-256 there.
5. Installs each component of the plan.
6. Writes `last-apply.json` and the result file.

The protected update folder is `%ProgramData%\amdgpu-wddm\update`. Administrators and SYSTEM can
change it. Users can only read it. The engine never reads a file twice from a folder that a user
can change.

The engine exits with 0 when it applied an update, found nothing to do or handed off a driver
update. It exits with 2 for a refusal, 3 for a rollback and 6 for a failure.

## The apps switch

`<InstallRoot>\control` becomes a junction to `<InstallRoot>\apps\<version>`. The first update
moves a classic `control` folder to `apps\<installed release>`.

`Switch-NagiApps` makes the switch in four steps. A journal, `apps\switch.json`, records each step.

1. `new`: makes the junction `control.new` to the new folder.
2. `moved`: moves `control` to `control.prev`.
3. `switched`: moves `control.new` to `control`.
4. `done`: removes `control.prev`.

`Repair-NagiAppsSwitch` reads the file system, not the journal step. When `control` is present,
it removes the helper links. When `control` is gone, it moves `control.new` to `control`. When
`control.new` is not there, it moves the old folder back.

After the switch, the engine starts the new control application with `--version`. When the
application does not answer in 30 seconds, the engine switches back and reports a rollback. The
engine keeps the current apps folder and the folder before it. It removes the other folders.

The normal installer also knows the junction. In that layout it copies the apps into
`apps\<release>` and switches the link.

## The driver transaction

A driver update writes `transaction.json` in the state `driver-pending`. The record holds the
`from` side and the `to` side: release, KMD hash, KMD ABI, state schema, catalog and apps folder.
Then the engine unpacks the release zip into `update\driver\<release>`. It checks the package
`manifest.json` against the update manifest. Then it hands off to the `setup.exe` of that package.

Two hooks record each start of the computer.

- `start-confirm.ps1` runs as SYSTEM at each start. It records the boot, the KMD hash of that boot
  and the BD-122 start confirmation code.
- The route check at the end of `install.ps1` records its result, `verified` or
  `verify-failed`.

`Get-NagiTxnDecision` reads the records of the boots.

| Decision | Condition | New state |
|---|---|---|
| commit | One boot of the new KMD has confirmation 0 or 6 and `verified` | `committed` |
| rollback | Two boots of the new KMD failed, and the old driver can take the state back | `rollback-pending` |
| manual | Two boots of the new KMD failed, but a rollback is not safe | `manual` |
| supersede | A third KMD ran | `superseded` |
| expire | No decision after 7 days | `expired` |
| wait | Any other case | `driver-pending` |

A failed boot has the confirmation code 1, 3 or 5, or `verify-failed`. A rollback is safe when
the state schema and the catalog of the two sides are the same. The apps must also speak the old
KMD ABI, or go back to their old folder too.

For a rollback, the hook calls `DiRollbackDriver` on the one device with the service `bc250kmd`.
Windows then installs the previous driver package again from the driver store. The new binding
applies at the next start. That start reads the KMD hash and sets `rolled-back` or `manual`.

The Driver page shows a line for the states `driver-pending`, `rollback-pending`, `rolled-back`
and `manual`.

## Failure modes

| Failure | Result |
|---|---|
| Bad signature, signer not pinned, or manifest not complete | Refused before a download of a pack |
| Bad size or SHA-256 of an asset | Refused, nothing installed |
| Package `manifest.json` differs from the signed manifest | Refused, no transaction |
| Downgrade, install too old, or ABI not compatible | Refused, with the reason in plain words |
| A control application file is locked | Refused as busy, the old apps stay |
| The new control application does not start | The apps switch back |
| A stop during the apps switch | The next run of the engine or the installer repairs the link |
| The new driver fails two starts | Rollback to the previous package, or the manual notice |
| The new driver never loads | No boot counts, and the transaction expires after 7 days |
| UAC prompt declined | Nothing changes, and the page says how to try again |

## Stage 2

Stage 2 adds a scheduled task, "Nagi Update", that runs as SYSTEM. It does the steps of the
control application without a UAC prompt, at a time that the user selects. Stage 2 is not
implemented. It needs its own design of consent, timing and the restart prompt.

## Deviations from the first design

- The driver asset is the full release zip, not a separate driver pack. The package's own
  `setup.exe` installs it on the tested install path. A second install path for the driver would
  need its own tests on each release.
- The apps link stays at the path `control`. Shortcuts, tasks and the uninstaller keep their paths.
- The rollback runs from the start confirmation task and from the route check of `install.ps1`. Stage 1 has no task
  of its own that could run it.
- Tester.36 has no self-update. The first update from inside the application is from tester.37 to
  the next release.
