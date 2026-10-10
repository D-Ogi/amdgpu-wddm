# Offline installer integration tests. Native tuning is replaced, even outside DryRun.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$outRoot = [IO.Path]::GetFullPath($Out)
[void][IO.Directory]::CreateDirectory($outRoot)
. (Join-Path $PSScriptRoot 'installer\common.ps1')
. (Join-Path $PSScriptRoot 'installer\system-tuning.ps1')
. (Join-Path $PSScriptRoot 'system-tuning-manifest.ps1')
$realTuningProcess = (Get-Item Function:\Invoke-TuningProcess).ScriptBlock
Import-Module (Join-Path $PSScriptRoot '..\win\system-tuning\SystemTuning.Core.psm1') -Force
$script:checks = 0
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message }; $script:checks++ }
function Refuses([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Assert $failed $Message
}
$script:planMessages = New-Object Collections.Generic.List[string]
function Write-Info([string]$Message) { $script:planMessages.Add($Message) }
function Invoke-Change([string]$Description, [scriptblock]$Action) { if (-not $script:DryRunMode) { & $Action } }
$script:calls = 0
$script:fake = @{ code = 0; text = '{"schema":1,"ok":true,"action":"ApplyRecommended","scope":"Machine","dryRun":false}'; error = '' }
function Invoke-TuningProcess([string]$ScriptPath, [string]$Action) { $script:calls++; return $script:fake }

$expected = @(Get-TuningCatalog | Where-Object recommended | ForEach-Object id)
$metadata = New-SystemTuningManifest -CoreModule (Join-Path $PSScriptRoot '..\win\system-tuning\SystemTuning.Core.psm1')
Assert (($expected -join ',') -ceq ($metadata.recommended -join ',')) 'Manifest and backend recommended choices disagree'
Assert (-not $metadata.selected_by_default -and $metadata.preserve_recovery_state) 'Manifest changed opt-in or recovery semantics'
Write-SystemTuningPlan -Selected $true -Manifest ([pscustomobject]@{system_tuning=$metadata})
foreach ($item in $metadata.recommended_items) { Assert (@($script:planMessages | Where-Object { $_.Contains($item.label) -and $_.Contains($item.id) -and $_.Contains($item.note) }).Count -eq 1) 'Plan omitted catalog item detail' }
# A different package catalog drives the installer plan without changing installer code.
$script:planMessages.Clear()
Write-SystemTuningPlan -Selected $true -Manifest ([pscustomobject]@{system_tuning=@{recommended_items=@(@{id='fixture.future';label='Future catalog item';note='Only the fixture changed.'})}})
Assert (@($script:planMessages | Where-Object { $_ -like '*fixture.future*' }).Count -eq 1) 'Installer retained its own recommendation list'
Refuses { Write-SystemTuningPlan -Selected $true -Manifest $null } 'Selected plan silently ignored missing catalog metadata'
Write-SystemTuningPlan -Selected $false -Manifest $null
Assert ($script:calls -eq 0) 'Plan invoked the tuning engine'
$none = Get-InstallInputs @{} $null '1'
Assert (-not ($none.switches -contains 'ApplySystemTuning')) 'Default must not select tuning'
$chosen = Get-InstallInputs @{ApplySystemTuning=$true} $null '1'
Assert ($chosen.switches -contains 'ApplySystemTuning') 'Explicit opt-in lost'
foreach ($phase in 'testsigning-pending','driver-pending-restart','install-incomplete','files-copied') {
    $state = [pscustomobject]@{ package_version='1'; phase=$phase; install_switches=@('ApplySystemTuning') }
    Assert ((Get-InstallInputs @{} $state '1').switches -contains 'ApplySystemTuning') "Resume lost opt-in: $phase"
    Assert (-not ((Get-InstallInputs @{SkipSystemTuning=$true} $state '1').switches -contains 'ApplySystemTuning')) "Resume cannot clear choice: $phase"
}
$state.phase = 'verified'
Assert (-not ((Get-InstallInputs @{} $state '1').switches -contains 'ApplySystemTuning')) 'Completed install must not opt into later tuning'
$state.phase = 'install-incomplete'
Assert (-not ((Get-InstallInputs @{} $state '2').switches -contains 'ApplySystemTuning')) 'New version must not inherit consent'
Refuses { Get-InstallInputs @{ApplySystemTuning=$true;SkipSystemTuning=$true} $null '1' } 'Contradictory input accepted'
$script:DryRunMode = $false
Invoke-SelectedSystemTuning -Selected $false -Action ApplyRecommended -PackageRoot 'missing-package'
Assert ($script:calls -eq 0) 'Unselected tuning invoked backend'
$script:DryRunMode = $true
Invoke-SelectedSystemTuning -Selected $true -Action ApplyRecommended -PackageRoot 'missing-package'
Invoke-SelectedSystemTuning -Selected $true -Action RestoreAll -PackageRoot 'missing-package'
Assert ($script:calls -eq 0) 'DryRun invoked backend or required installed closure'
$script:DryRunMode = $false
$package = Join-Path $outRoot ('fake-package-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $package 'payload\system-tuning'
[void][IO.Directory]::CreateDirectory($payload)
$files = @()
foreach ($name in $script:SystemTuningFiles) {
    $p = Join-Path $payload $name
    [IO.File]::WriteAllText($p, '# Test fixture, never executed.')
    $files += @{ path='payload/system-tuning/'+$name; sha256=(Get-FileHash -LiteralPath $p).Hash }
}
@{ files=$files } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
Invoke-SelectedSystemTuning -Selected $true -Action ApplyRecommended -PackageRoot $package
Assert ($script:calls -eq 1) 'Explicit opt-in did not invoke backend once'
$script:fake.text = '{"schema":1,"ok":false,"error":"test refusal","action":"RestoreAll","dryRun":false}'
$script:fake.code = 1
$script:removed = $false
Refuses { Invoke-SelectedSystemTuning -Selected $true -Action RestoreAll -PackageRoot $package; $script:removed=$true } 'Restore failure hidden'
Assert (-not $script:removed) 'Removal continued after restore failure'
foreach ($json in '{"schema":1,"ok":false,"action":"ApplyRecommended"}', '{"schema":1,"ok":true,"action":"RestoreAll"}', '{"schema":1,"ok":true,"action":"ApplyRecommended","dryRun":true}', '{"schema":1,"ok":"true","action":"ApplyRecommended"}', 'not-json') {
    $script:fake.code=0; $script:fake.text=$json
    Refuses { Invoke-SelectedSystemTuning -Selected $true -Action ApplyRecommended -PackageRoot $package } 'Malformed success accepted'
}
$before = $script:calls
[IO.File]::AppendAllText((Join-Path $payload 'SystemTuning.Native.psm1'), 'tampered')
Refuses { Invoke-SelectedSystemTuning -Selected $true -Action ApplyRecommended -PackageRoot $package } 'Changed helper accepted'
Assert ($script:calls -eq $before) 'Changed helper reached execution'
# Installed path uses the same manifest entries and preserves nested directories.
$mapped = Get-InstalledPathOfPackageFile -PackagePath 'payload/control/system-tuning/SystemTuning.Core.psm1' -InstallRoot $package
Assert ($mapped -eq (Join-Path $package 'control\system-tuning\SystemTuning.Core.psm1')) 'Installed path lost nested closure'
$user = Join-Path $outRoot ('user-data-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $user 'system-tuning'))
[void][IO.Directory]::CreateDirectory((Join-Path $user 'shader-cache'))
[IO.File]::WriteAllText((Join-Path $user 'system-tuning\state.json'), 'private recovery fixture')
$script:deleted = New-Object Collections.Generic.List[string]
function Remove-PathOrSchedule([string]$Path) { $script:deleted.Add($Path) }
Remove-UserDataExceptTuning $user
Assert ($script:deleted.Count -eq 1 -and $script:deleted[0] -eq (Join-Path $user 'shader-cache')) 'Uninstall removed a user recovery journal'
Assert ([IO.File]::ReadAllText((Join-Path $user 'system-tuning\state.json')) -ceq 'private recovery fixture') 'Uninstall changed user recovery contents'
# Exercise the actual child-process transport with a harmless fixture, including spaces in its path.
$child = Join-Path $outRoot 'fake tuning child.ps1'
[IO.File]::WriteAllText($child, 'param($Action,$Scope) @{schema=1;ok=$true;action=$Action;scope=$Scope;dryRun=$false}|ConvertTo-Json -Compress')
$native = & $realTuningProcess -ScriptPath $child -Action ApplyRecommended
$parsed = $native.text | ConvertFrom-Json
Assert ($native.code -eq 0 -and $parsed.scope -ceq 'Machine' -and $parsed.action -ceq 'ApplyRecommended') 'Real child invocation lost action or machine scope'
[IO.File]::WriteAllText($child, 'Start-Sleep -Seconds 20')
$clock = [Diagnostics.Stopwatch]::StartNew()
Refuses { & $realTuningProcess -ScriptPath $child -Action ApplyRecommended -TimeoutSeconds 1 } 'Hung child was not refused'
Assert ($clock.Elapsed.TotalSeconds -lt 8) 'Child timeout or drain exceeded its bound'
# Run the actual uninstall script with a fake host. A declined removal must stop before every mutation.
$decline = Join-Path $outRoot ('decline-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $decline 'installer'))
foreach ($name in 'uninstall.ps1','system-tuning.ps1') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "installer\$name") -Destination (Join-Path $decline "installer\$name") }
[IO.File]::WriteAllText((Join-Path $decline 'installer\mft-h264.ps1'), '# empty fixture')
$fakeCommon = @'
$script:SoftwareKey='HKLM:\SOFTWARE\unused-test-key'
function Test-IsAdmin { $true }
function Read-InstallState { [pscustomobject]@{install_root=$PSScriptRoot} }
function Write-Info($s) { }
function Get-LabInstallPaths { @() }
function Read-Confirmation { $false }
function Invoke-Change { throw 'MUTATION BEFORE CONSENT' }
'@
[IO.File]::WriteAllText((Join-Path $decline 'installer\common.ps1'), $fakeCommon)
$hostExe = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$declined = Invoke-Native -File $hostExe -Arguments @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $decline 'installer\uninstall.ps1'),'-RestoreSystemTuning')
Assert ($declined.code -eq 4 -and $declined.text -match 'Nothing was changed' -and $declined.text -notmatch 'MUTATION') 'Declining removal changed Windows settings or kept recovery tools'
Write-Host "PASS: $script:checks system-tuning installer checks. No Windows settings changed."
