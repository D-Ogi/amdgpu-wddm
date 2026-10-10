# Windows system tuning

This tool records original settings before each change. Restore uses that record, not assumed Windows defaults.
Machine records survive application updates in `%ProgramData%\amdgpu-wddm\system-tuning`.
User records reside in `%LocalAppData%\amdgpu-wddm\system-tuning`.

## Commands

Run the script with 64-bit Windows PowerShell 5.1.
Use elevation for machine changes. Run user changes directly under the intended account.
Never elevate user operations through another account.

```powershell
.\system-tuning.ps1 -Action List -Scope Machine
.\system-tuning.ps1 -Action List -Scope User
.\system-tuning.ps1 -Action Apply -Item service.SysMain -DryRun
.\system-tuning.ps1 -Action ApplyRecommended -Scope Machine
.\system-tuning.ps1 -Action Restore -Item service.SysMain
.\system-tuning.ps1 -Action Apply -Scope User -Item autostart.OneDrive
.\system-tuning.ps1 -Action RestoreAll -Scope User
.\system-tuning.ps1 -Action PauseUpdates -PauseDays 7
.\system-tuning.ps1 -Action ResumeUpdates
.\system-tuning.ps1 -Action Apply -Item policy.DriverUpdates
```

`Machine` is the default scope. `List` and `DryRun` create no directories or records.
`RestoreAll` restores only entries managed in the selected scope.
It continues after individual failures and retains failed entries.
Repeated application preserves the first original state.
The tool refuses restoration when another program changed the managed setting.

## Catalog

| ID | Scope | Recommended | Effect |
| --- | --- | --- | --- |
| `service.SysMain` | Machine | No | Disable preloading service |
| `service.WSearch` | Machine | No | Disable indexing service |
| `service.DiagTrack` | Machine | Yes | Disable diagnostic telemetry service |
| `service.MapsBroker` | Machine | Yes | Disable downloaded maps service |
| `task.ScheduledDefrag` | Machine | No | Disable scheduled drive optimization |
| `task.CompatibilityAppraiser` | Machine | No | Disable compatibility telemetry task |
| `task.ProgramDataUpdater` | Machine | Yes | Disable program telemetry task |
| `task.Consolidator` | Machine | Yes | Disable experience telemetry task |
| `task.UsbCeip` | Machine | Yes | Disable USB telemetry task |
| `autostart.TeamsMachineInstaller` | Machine | No | Remove legacy Teams installer autostart |
| `autostart.OneDrive` | User | No | Remove OneDrive autostart |
| `autostart.Teams` | User | No | Remove classic Teams autostart |
| `policy.DriverUpdates` | Machine | No | Exclude driver offers from quality updates |
| `policy.UpdatePause` | Machine | No | Configure a finite update pause |

Scheduled optimization includes SSD maintenance. Disable it only after considering that cost.
Removing OneDrive autostart requires a manual start for synchronization.
No operation removes applications, kills processes, disables security, or changes network services.
No operation stops Windows Update or modifies browser updater tasks.
Dynamic browser autostart names are outside this catalog.

## Update policies

The update controls require a supported Pro, Enterprise, or Education edition.
The tool refuses new update changes on domain-managed machines or when MDM explicitly overrides group policies.
Other organization policies can still override local configuration.

The pause uses `PauseQualityUpdatesStartTime` and `PauseFeatureUpdatesStartTime` as date strings.
Microsoft defines expiration as the configured start date plus 35 days.
For a requested duration, the tool subtracts the unused days from the UTC calendar date.
The range is 1 through 35 days. Windows owns expiration, without an application timer or resident task.
Pause delays quality security updates too. It does not guarantee cancellation of an installation already running.

`ResumeUpdates` restores the previous pause policy. That previous policy can itself contain a pause.
The response separates configured dates from observed Windows pause status.
A successful registry readback does not prove that Windows has processed the policy.

Driver exclusion affects driver offers within quality updates. It applies to all devices, not only this driver.
It does not exclude critical OS drivers or drivers delivered with feature updates.
It does not guarantee permanent protection against replacement.

Contracts: [update configuration](https://learn.microsoft.com/en-us/windows/deployment/update/waas-configure-wufb)
and [Update Policy CSP](https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-update).
The deployment table mislabels pause date values as DWORDs. The CSP defines these dates as strings.

## Recovery record

Each mutation holds an exclusive file lock. The record precedes the operating system change.
The tool writes a temporary file, flushes it, then atomically replaces the record.
It uses DPAPI authentication and encryption. Machine protection uses `LocalMachine`. User protection uses `CurrentUser`.
The journal also binds to the machine name. User records additionally bind to the account SID.

Machine directories permit writes only from Administrators and SYSTEM.
User directories additionally permit the current account.
Existing records and directories must pass owner, ACL, and reparse checks.
The tool rejects corrupt records instead of rebuilding them from current settings.
The installed command has no record-directory override.

The journal stores no executable operation or destination path.
Only fixed catalog descriptors select services, tasks, and registry values.
Autostart restoration restores the authenticated original string at its fixed value name.
It never executes that string.
Preserve the records and recovery scripts when removing the application.

Service records include startup mode, running state, and exact delayed-start value presence.
The service manager receives and verifies delayed-start changes through `ChangeServiceConfig2` and `QueryServiceConfig2`.
The tool also preserves original registry value presence. It refuses changes when the registry and service manager disagree.
This flag controls the next boot, as specified by the
[delayed-start contract](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_delayed_auto_start_info).
The stop request cannot force dependent services to stop.
Task records preserve the enabled flag. A running task is not stopped.
Autostart values preserve their original string type without expanding environment variables.

## Legacy records

Use explicit machine import to adopt complete records from the older laboratory tool.

```powershell
.\system-tuning.ps1 -Action ImportLegacy -LegacyStatePath .\debloat-state.json -DryRun
.\system-tuning.ps1 -Action ImportLegacy -LegacyStatePath .\debloat-state.json
```

Import changes only the recovery journal. It never changes Windows settings.
The schema must be `bc250-llmvram-debloat/1`, with a matching host and a false `what_if_only` field.
Only fixed catalog targets qualify. Existing originals cannot be replaced.
The current setting must match the recorded disabled state.

Older service records lack `DelayedAutoStart`, so their import fails without inventing a value.
Legacy autostart commands lack authentication, so their import also fails.
Security, updater, and unknown targets are excluded.
Complete task records remain eligible, including `ScheduledDefrag`.
Partial import retains accepted entries and reports each rejection.

## JSON interface

Standard output contains one UTF-8 JSON object. Exit code zero means `ok:true`.
The root fields are `schema`, `ok`, `error`, `action`, `scope`, `dryRun`, `items`, and `results`.
`schema` is 1. Import also reports `importedCount`, `rejectedCount`, and `excludedCount`.
Each result contains `id`, `ok`, `status`, and `error`.

Each item contains `id`, `kind`, `label`, `recommended`, `present`, `current`, `managed`, `canApply`, `canRestore`, and `note`.
`current` is a typed snapshot, or null when unreadable. Clients must not display its raw autostart command.
The additional fields are `state`, `displayState`, `requestedUntil`, `observed`, and `observedDisplayState`.

`state` is `unmanaged`, `applying`, `applied`, `applyFailed`, `restoring`, or `restoreFailed`.
`displayState` is `absent`, `unavailable`, `enabled`, `disabled`, `mixed`, `configured`, or `notConfigured`.
`requestedUntil` is a `yyyy-MM-dd` date or null.
`observedDisplayState` is `paused`, `resumed`, `mixed`, or `unknown`.
Observed quality and feature status values are 0 for unpaused, 1 for paused, 2 for resumed, or null.

Use IDs and these finite states for localized text. Do not display raw technical errors or paths as interface instructions.

## Tests

Run the core tests against injected operations. They do not read or change host settings.

```powershell
.\test-system-tuning.ps1 -Out .\test-output
.\test-system-tuning-journal.ps1 -Out .\test-output
```

The second test exercises only files, DPAPI, and ACLs inside the specified output directory.
It needs a normal user token that can use the tested ACLs. A restricted sandbox token can fail that requirement.
Neither test calls the native settings reader or writer.
