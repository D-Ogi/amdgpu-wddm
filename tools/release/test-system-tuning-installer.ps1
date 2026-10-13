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
# The Nagi wallpaper picture stays too (owner, 2026-10-13): it can be the background of that account. A folder that
# holds only the picture keeps its root, and a folder with neither kept folder goes as a whole.
$userWp = Join-Path $outRoot ('user-data-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $userWp 'wallpaper'))
[void][IO.Directory]::CreateDirectory((Join-Path $userWp 'vkd3d'))
[IO.File]::WriteAllText((Join-Path $userWp 'wallpaper\nagi-wallpaper.jpg'), 'picture fixture')
$script:deleted.Clear()
Remove-UserDataExceptTuning $userWp
Assert ($script:deleted.Count -eq 1 -and $script:deleted[0] -eq (Join-Path $userWp 'vkd3d')) 'Uninstall removed the Nagi wallpaper picture'
$userNone = Join-Path $outRoot ('user-data-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $userNone 'vkd3d'))
$script:deleted.Clear()
Remove-UserDataExceptTuning $userNone
Assert ($script:deleted.Count -eq 1 -and $script:deleted[0] -eq [IO.Path]::GetFullPath($userNone)) 'Uninstall kept a folder with nothing to keep'
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
# Run the actual uninstall script with a fake host. The uninstaller asks nothing (setup-v2, c9825005: running
# uninstall.cmd is the answer, and Read-Confirmation is gone from common.ps1), so a run without -Yes goes on to its
# first change. The fixture's Read-Confirmation throws, so a question that comes back is a finding here.
$decline = Join-Path $outRoot ('decline-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $decline 'installer'))
foreach ($name in 'uninstall.ps1','system-tuning.ps1','components.ps1') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "installer\$name") -Destination (Join-Path $decline "installer\$name") }
[IO.File]::WriteAllText((Join-Path $decline 'installer\mft-h264.ps1'), '# empty fixture')
$fakeCommon = @'
$script:SoftwareKey='HKLM:\SOFTWARE\unused-test-key'
function Test-IsAdmin { $true }
function Read-InstallState { [pscustomobject]@{install_root=$PSScriptRoot} }
function Write-Info($s) { }
function Write-Fail($s) { Write-Output ('FAIL: ' + $s) }
function Get-LabInstallPaths { @() }
function Get-OurDriverPackages { @() }
function Read-Confirmation { throw 'QUESTION ASKED' }
function Invoke-Change { throw 'MUTATION REACHED' }
'@
[IO.File]::WriteAllText((Join-Path $decline 'installer\common.ps1'), $fakeCommon)
$hostExe = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$unasked = Invoke-Native -File $hostExe -Arguments @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $decline 'installer\uninstall.ps1'),'-RestoreSystemTuning')
Assert ($unasked.text -match 'MUTATION REACHED' -and $unasked.text -notmatch 'QUESTION ASKED' -and $unasked.text -notmatch 'Nothing was changed') 'The uninstaller asked a question, or stopped before its first change, in a run without -Yes'
# An unreadable driver store stops before tuning restoration or recovery copies, even with explicit consent.
$failedInventory = $fakeCommon.Replace('function Get-OurDriverPackages { @() }', "function Get-OurDriverPackages { throw 'fixture inventory failure' }")
[IO.File]::WriteAllText((Join-Path $decline 'installer\common.ps1'), $failedInventory)
$refused = Invoke-Native -File $hostExe -Arguments @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $decline 'installer\uninstall.ps1'),'-Yes','-RestoreSystemTuning')
Assert ($refused.code -eq 6 -and $refused.text -match 'No removal was started' -and $refused.text -match 'fixture inventory failure' -and $refused.text -notmatch 'MUTATION') 'Failed inventory did not stop before Windows tuning or recovery-file changes'
# ---------------------------------------------------------------------------------------------------------------
# BD-129: the recovery save of an uninstall checked the shared parent %ProgramData%\amdgpu-wddm as strictly as the
# recovery directory itself, and refused it for the rules it inherits from ProgramData (Users may add a folder,
# CREATOR OWNER). The installer creates that parent itself, so the uninstaller of 0.7.216.100-tester.28 stopped
# before it removed anything, on every machine. These cases build the same shapes in a temporary ProgramData.
#
# A test host is not elevated and cannot give a directory to another account, not even to a group of its own token
# (measured: Set-Acl refuses the owner change). The trusted list of the installer code is the seam instead: with
# this account in it, a directory of this account stands for a directory of Administrators; with the shipping list,
# the same directory stands for one of a foreign owner.
Assert (($script:SystemTuningTrustedSids -join ',') -ceq 'S-1-5-18,S-1-5-32-544') 'The shipping trusted-owner list is not SYSTEM and Administrators'
$me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$shippingTrusted = $script:SystemTuningTrustedSids
$withMe = @('S-1-5-18','S-1-5-32-544',$me)
$savedProgramData = $env:ProgramData
$bd129 = Join-Path $outRoot 'bd129'
[void][IO.Directory]::CreateDirectory($bd129)
# A package fixture of its own: the one above has a tampered helper on purpose.
$recoveryPackage = Join-Path $bd129 'package'
$recoveryPayload = Join-Path $recoveryPackage 'payload\system-tuning'
[void][IO.Directory]::CreateDirectory($recoveryPayload)
$recoveryFiles = @()
foreach ($name in $script:SystemTuningFiles) {
    $p = Join-Path $recoveryPayload $name
    [IO.File]::WriteAllText($p, "# Recovery fixture of $name, never executed.")
    $recoveryFiles += @{ path='payload/system-tuning/'+$name; sha256=(Get-FileHash -LiteralPath $p).Hash }
}
@{ files=$recoveryFiles } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $recoveryPackage 'manifest.json') -Encoding UTF8
function New-Bd129ProgramData([string]$Case) {
    $root = Join-Path $bd129 $Case
    [void][IO.Directory]::CreateDirectory($root)
    return $root
}
# The rules a directory under ProgramData inherits: Users may add a folder in the container, CREATOR OWNER owns
# what it creates. Nothing here removes a rule, as the installer must not on the real parent either.
function Add-InheritedProgramDataRules([string]$Path) {
    $acl = Get-Acl -LiteralPath $Path
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-545'),[Security.AccessControl.FileSystemRights]::AppendData,'ContainerInherit','None','Allow')))
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier 'S-1-3-0'),'FullControl','ContainerInherit,ObjectInherit','InheritOnly','Allow')))
    Set-Acl -LiteralPath $Path -AclObject $acl
}
# The protected shape the recovery directory is created with, with this account in place of Administrators.
function New-Bd129Protected([string]$Path, $ExtraRule) {
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier $me))
    $acl.SetAccessRuleProtection($true,$false)
    foreach ($sid in 'S-1-5-18','S-1-5-32-544',$me) {
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier $sid),'FullControl','ContainerInherit,ObjectInherit','None','Allow')))
    }
    $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-545'),'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')))
    if ($ExtraRule) { $acl.AddAccessRule($ExtraRule) }
    [void](New-Object IO.DirectoryInfo $Path).Create($acl)
}
function Get-Bd129Refusal([scriptblock]$Action) {
    try { & $Action; return '' } catch { return [string]$_.Exception.Message }
}
try {
    # 1. The BD-129 shape itself: a parent that carries the inherited ProgramData rules. The save must succeed and
    #    must not change that parent.
    $case = New-Bd129ProgramData 'parent-inherited'
    $parent = Join-Path $case 'amdgpu-wddm'
    [void][IO.Directory]::CreateDirectory($parent)
    Add-InheritedProgramDataRules $parent
    $child = Join-Path $parent 'system-tuning-recovery'
    New-Bd129Protected $child $null
    $parentBefore = (Get-Acl -LiteralPath $parent).Sddl
    $env:ProgramData = $case
    $script:SystemTuningTrustedSids = $withMe
    $script:planMessages.Clear()
    Save-SystemTuningRecovery -PackageRoot $recoveryPackage
    foreach ($name in $script:SystemTuningFiles) {
        $copy = Join-Path $child $name
        Assert ([IO.File]::Exists($copy)) "The recovery save left out $name"
        Assert ((Get-FileHash -LiteralPath $copy).Hash -ceq (Get-FileHash -LiteralPath (Join-Path $recoveryPayload $name)).Hash) "The recovery copy of $name differs from the package file"
    }
    Assert ((Get-Acl -LiteralPath $parent).Sddl -ceq $parentBefore) 'The recovery save changed the access rules of the shared parent'
    Assert (@($script:planMessages | Where-Object { $_.Contains($child) }).Count -eq 1) 'The recovery save did not report where the tools are'

    # 2. A parent that is a reparse point: refused, and nothing is written through it.
    $case = New-Bd129ProgramData 'parent-reparse'
    $elsewhere = Join-Path $case 'elsewhere'
    [void][IO.Directory]::CreateDirectory($elsewhere)
    $parent = Join-Path $case 'amdgpu-wddm'
    & cmd.exe /c mklink /J "$parent" "$elsewhere" | Out-Null
    Assert (((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) 'The fixture parent is not a reparse point'
    $env:ProgramData = $case
    $message = Get-Bd129Refusal { Save-SystemTuningRecovery -PackageRoot $recoveryPackage }
    Assert ($message -match 'is not a regular directory' -and $message.Contains($parent)) "A reparse-point parent was accepted: '$message'"
    Assert (-not (Test-Path -LiteralPath (Join-Path $elsewhere 'system-tuning-recovery'))) 'The recovery save wrote through a reparse-point parent'

    # 3. A parent of a foreign owner: refused. The shipping trusted list makes this account the foreign owner.
    $case = New-Bd129ProgramData 'parent-owner'
    $parent = Join-Path $case 'amdgpu-wddm'
    [void][IO.Directory]::CreateDirectory($parent)
    Add-InheritedProgramDataRules $parent
    $env:ProgramData = $case
    $script:SystemTuningTrustedSids = $shippingTrusted
    $message = Get-Bd129Refusal { Save-SystemTuningRecovery -PackageRoot $recoveryPackage }
    Assert ($message -match 'has an unexpected owner' -and $message.Contains($parent) -and $message.Contains($me)) "A parent of a foreign owner was accepted: '$message'"
    Assert (-not (Test-Path -LiteralPath (Join-Path $parent 'system-tuning-recovery'))) 'The recovery save created its directory under a parent it refused'

    # 4. The recovery directory itself keeps the strict rule. Its owner is checked against the same list: with the
    #    shipping list this account is foreign. One list cannot hold a trusted parent and a foreign child at once,
    #    so this case asks the check that the save runs, with the strict form it uses for the child.
    $case = New-Bd129ProgramData 'child-owner'
    $parent = Join-Path $case 'amdgpu-wddm'
    [void][IO.Directory]::CreateDirectory($parent)
    $child = Join-Path $parent 'system-tuning-recovery'
    New-Bd129Protected $child $null
    $message = Get-Bd129Refusal { Assert-SystemTuningRecoveryDirectory $child $true }
    Assert ($message -match 'has an unexpected owner' -and $message.Contains($child)) "A recovery directory of a foreign owner was accepted: '$message'"
    $script:SystemTuningTrustedSids = $withMe
    Assert ((Get-Bd129Refusal { Assert-SystemTuningRecoveryDirectory $child $true }) -ceq '') 'The same recovery directory of a trusted owner was refused'

    # 5. A recovery directory that another account may write: refused, even with a trusted owner and a parent that
    #    passes. This is the rule the parent no longer carries.
    $case = New-Bd129ProgramData 'child-write'
    $parent = Join-Path $case 'amdgpu-wddm'
    [void][IO.Directory]::CreateDirectory($parent)
    Add-InheritedProgramDataRules $parent
    $child = Join-Path $parent 'system-tuning-recovery'
    New-Bd129Protected $child (New-Object Security.AccessControl.FileSystemAccessRule ((New-Object Security.Principal.SecurityIdentifier 'S-1-5-32-545'),'Modify','ContainerInherit,ObjectInherit','None','Allow'))
    $env:ProgramData = $case
    $message = Get-Bd129Refusal { Save-SystemTuningRecovery -PackageRoot $recoveryPackage }
    Assert ($message -match 'grants write access to another account' -and $message.Contains($child) -and $message.Contains('S-1-5-32-545')) "A writable recovery directory was accepted: '$message'"
    foreach ($name in $script:SystemTuningFiles) { Assert (-not [IO.File]::Exists((Join-Path $child $name))) "The recovery save copied $name into a directory it refused" }
} finally {
    $env:ProgramData = $savedProgramData
    $script:SystemTuningTrustedSids = $shippingTrusted
}
Assert (($script:SystemTuningTrustedSids -join ',') -ceq 'S-1-5-18,S-1-5-32-544') 'The trusted-owner list was left overridden'
Write-Host "PASS: $script:checks system-tuning installer checks. No Windows settings changed."
