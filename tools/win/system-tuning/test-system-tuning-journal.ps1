#requires -Version 5.1
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
# Real file/DPAPI tests only. No Read/Write provider operation is called.
Import-Module (Join-Path $PSScriptRoot 'SystemTuning.Native.psm1') -Force
$module=Get-Module SystemTuning.Native
$ops=New-NativeTuningOperations -Scope User
[void][IO.Directory]::CreateDirectory($Out)
$testRoot=Join-Path ([IO.Path]::GetFullPath($Out)) ('journal-'+[Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$recordRoot=Join-Path $testRoot 'amdgpu-wddm\system-tuning'
# Private module injection exists only here. The installed CLI exposes no path override.
& $module {param($Root)$script:Root=$Root} $recordRoot
$checks=0
function Check([bool]$Value,[string]$Message){$script:checks++;if(-not $Value){throw ('Journal test failed: '+$Message)}}
$previousTemp=$env:TEMP;$previousTmp=$env:TMP
try{
    $env:TEMP=$testRoot;$env:TMP=$testRoot
    & $module {Initialize-ServiceInterop}
    Check ($null -ne ('AmdgpuWddm.SystemTuning.ServiceConfig' -as [type])) 'service interop compiles without calling native APIs'
    $refused=$false
    try{$null=[AmdgpuWddm.SystemTuning.ServiceConfig]::Delayed('outside-fixed-catalog',$null)}catch{$refused=$true}
    Check $refused 'interop refuses foreign service before opening SCM'
}finally{$env:TEMP=$previousTemp;$env:TMP=$previousTmp}
$empty=& $module {Read-TuningJournal}
Check ($null -eq $empty -and -not [IO.Directory]::Exists((Split-Path -Parent $recordRoot))) 'read does not create directories'
$lock=& $module {Lock-TuningState}
try{
    $refused=$false
    try{$second=& $module {Lock-TuningState};$second.Dispose()}catch{$refused=$true}
    Check $refused 'concurrent lock refused'
    $record=@{schema=1;machine='fixture';entries=@{};unicode='Za' + [char]0x17c + [char]0xf3 + [char]0x142 + [char]0x107}
    & $module {param($Record)Write-TuningJournal $Record} $record
    $loaded=& $module {Read-TuningJournal}
    Check ($loaded.schema -eq 1 -and $loaded.unicode -ceq $record.unicode) 'DPAPI CurrentUser exact round-trip'
    $record.machine='replacement'
    & $module {param($Record)Write-TuningJournal $Record} $record
    $loaded=& $module {Read-TuningJournal}
    Check ($loaded.machine -ceq 'replacement') 'atomic replacement readable'
    Check (@(Get-ChildItem -LiteralPath $recordRoot -Filter '*.tmp').Count -eq 0) 'temporary journal files cleaned'
    & $module {$script:Protection=[Security.Cryptography.DataProtectionScope]::LocalMachine}
    & $module {param($Record)Write-TuningJournal $Record} $record
    $loaded=& $module {Read-TuningJournal}
    Check ($loaded.machine -ceq 'replacement') 'DPAPI LocalMachine exact round-trip'
    $path=Join-Path $recordRoot 'state.dpapi'
    $bytes=[IO.File]::ReadAllBytes($path);$bytes[[int]($bytes.Length/2)]=$bytes[[int]($bytes.Length/2)] -bxor 1
    [IO.File]::WriteAllBytes($path,$bytes)
    $refused=$false
    try{$null=& $module {Read-TuningJournal}}catch{$refused=$true}
    Check $refused 'corrupt authenticated record refused'
    # Refuse a writable existing file instead of silently repairing its ACL.
    $acl=[IO.File]::GetAccessControl($path,[Security.AccessControl.AccessControlSections]::Access)
    $everyone=New-Object Security.Principal.SecurityIdentifier('S-1-1-0')
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($everyone,'Write','Allow'))
    [IO.File]::SetAccessControl($path,$acl)
    $refused=$false
    try{& $module {param($Path)Assert-ProtectedPath $Path} $path}catch{$refused=$true}
    Check $refused 'weak file ACL refused'
}finally{$lock.Dispose()}
$weak=Join-Path $testRoot 'weak'
& $module {param($Path)New-ProtectedDirectory $Path} $weak
$acl=[IO.Directory]::GetAccessControl($weak,[Security.AccessControl.AccessControlSections]::Access)
$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($everyone,'Write','ContainerInherit,ObjectInherit','None','Allow'))
[IO.Directory]::SetAccessControl($weak,$acl)
$refused=$false
try{& $module {param($Path)New-ProtectedDirectory $Path} $weak}catch{$refused=$true}
Check $refused 'weak existing directory refused'
$junction=Join-Path $testRoot 'junction'
$junctionTest='passed'
try{
    $null=New-Item -ItemType Junction -Path $junction -Target $recordRoot
    $refused=$false
    try{& $module {param($Path)Assert-ProtectedPath $Path} $junction}catch{$refused=$true}
    Check $refused 'reparse point refused'
}catch{
    if($_.Exception.Message -like 'Journal test failed:*'){throw}
    $junctionTest='unavailable: '+$_.Exception.GetType().Name
}
$report=@{schema=1;ok=$true;checks=$checks;nativeOperationsExecuted=0;journalIo=$true;junction=$junctionTest}
[IO.File]::WriteAllText((Join-Path $Out 'system-tuning-journal-tests.json'),($report | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Write-Output ('System tuning journal tests passed: '+$checks+' ('+$junctionTest+')')
