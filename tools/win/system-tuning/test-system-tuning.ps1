#requires -Version 5.1
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
Import-Module (Join-Path $PSScriptRoot 'SystemTuning.Core.psm1') -Force
# This test never imports or calls the native adapter. All settings and journals are in memory.
$script:Checks=0
function Check([bool]$Condition,[string]$Message){$script:Checks++;if(-not $Condition){throw "Check failed: $Message"}}
function Copy-Value($Value){if($null -eq $Value){return $null};return ConvertTo-TuningMap ($Value | ConvertTo-Json -Depth 18 -Compress | ConvertFrom-Json)}
function Equal-Value($A,$B){
    if($null -eq $A -or $null -eq $B){return ($null -eq $A -and $null -eq $B)}
    if($A -is [System.Collections.IDictionary]){
        if($B -isnot [System.Collections.IDictionary] -or $A.Count -ne $B.Count){return $false}
        foreach($key in $A.Keys){if(-not $B.Contains($key) -or -not (Equal-Value $A[$key] $B[$key])){return $false}}
        return $true
    }
    return ($A.GetType() -eq $B.GetType() -and $A -ceq $B)
}
function New-Fake([string]$Scope='Machine'){
    $script:Fake=@{states=@{};journal=$null;saves=0;writes=0;locks=0;unlocks=0;locked=$false;admin=$true;failSave=0;failWrite='';partial=$false;verifyFail='';now=[datetime]'2026-12-31T12:00:00Z';readUnsupported='';observed=@{qualityStatus=0;featureStatus=0}}
    foreach($d in @(Get-TuningCatalog -Scope $Scope)){
        $s=switch($d.kind){
            service {@{present=$true;start=2;running=$true;delayed=@{exists=$true;kind='DWord';data=1}}}
            task {@{present=$true;enabled=$true}}
            autostart {@{present=$true;value=@{exists=$true;kind='ExpandString';data='%LOCALAPPDATA%\Example\Teams.exe --startup'}}}
            driverPolicy {@{present=$true;exclude=@{exists=$false;kind='DWord';data=$null}}}
            pausePolicy {@{present=$true;quality=@{exists=$false;kind='String';data=$null};feature=@{exists=$false;kind='String';data=$null}}}
        }
        $script:Fake.states[$d.id]=$s
    }
    return @{
        Scope=$Scope;Machine='FAKE-MACHINE'
        IsAdmin={if($script:FakeScope -eq 'User'){$true}else{$script:Fake.admin}}
        Now={$script:Fake.now}
        Load={Copy-Value $script:Fake.journal}
        Save={param($Journal)$script:Fake.saves++;if($script:Fake.failSave -eq $script:Fake.saves){throw 'Injected journal write failure.'};$script:Fake.journal=Copy-Value $Journal}
        Acquire={if($script:Fake.locked){throw 'Injected lock conflict.'};$script:Fake.locked=$true;$script:Fake.locks++;return 'LOCK'}
        Release={param($Lock)$script:Fake.locked=$false;$script:Fake.unlocks++}
        Read={param($D)@{state=(Copy-Value $script:Fake.states[$D.id]);supported=($script:Fake.readUnsupported -ne $D.id);note='';observed=$script:Fake.observed}}
        Write={param($D,$State)
            if(-not $script:Fake.journal.entries.Contains($D.id)){throw 'Write happened without durable original.'}
            if($script:Fake.journal.entries[$D.id].phase -notin @('applying','restoring')){throw 'Write happened without pending phase.'}
            $script:Fake.writes++
            if($script:Fake.failWrite -eq $D.id){
                if($script:Fake.partial){
                    if($D.kind -eq 'service'){$script:Fake.states[$D.id].running=$State.running}
                    elseif($D.kind -eq 'pausePolicy'){$script:Fake.states[$D.id].quality=Copy-Value $State.quality}
                }
                throw 'Injected settings write failure.'
            }
            if($script:Fake.verifyFail -ne $D.id){$script:Fake.states[$D.id]=Copy-Value $State}
        }
    }
}
$script:FakeScope='Machine'
$ops=New-Fake
$r=Invoke-SystemTuning -Ops $ops
Check $r.ok 'readonly List succeeds'
Check ($r.items.Count -eq 12) 'fixed machine catalog count'
Check ($script:Fake.saves -eq 0 -and $script:Fake.locks -eq 0 -and $script:Fake.writes -eq 0) 'List writes nothing'
Check (@($r.items | Where-Object recommended).Count -eq 5) 'five conservative recommendations'
foreach($action in @('Apply','Restore','RestoreAll','ApplyRecommended','PauseUpdates','ResumeUpdates')){
    $r=Invoke-SystemTuning -Ops $ops -Action $action -Item 'service.SysMain' -DryRun
    Check $r.ok ('dry run '+$action)
}
Check ($script:Fake.saves -eq 0 -and $script:Fake.locks -eq 0 -and $script:Fake.writes -eq 0) 'every dry run writes nothing'

foreach($id in @('service.SysMain','task.ScheduledDefrag','autostart.TeamsMachineInstaller','policy.DriverUpdates')){
    $ops=New-Fake;$original=Copy-Value $script:Fake.states[$id]
    $r=Invoke-SystemTuning -Ops $ops -Action Apply -Item $id
    Check $r.ok ('apply '+$id)
    $first=$script:Fake.journal | ConvertTo-Json -Depth 16 -Compress
    $r=Invoke-SystemTuning -Ops $ops -Action Apply -Item $id
    # Removed startup values remain present as managed operations, so repeated apply is idempotent.
    Check $r.ok ('repeat apply '+$id)
    if($id -like 'autostart.*'){Check ($script:Fake.journal.entries[$id].original.value.data -ceq $original.value.data) 'startup original preserved'}
    $r=Invoke-SystemTuning -Ops $ops -Action Restore -Item $id
    Check $r.ok ('restore '+$id)
    Check ($script:Fake.journal.entries.Count -eq 0) ('restored entry removed '+$id)
    Check (Equal-Value $script:Fake.states[$id] $original) ('exact original restored '+$id)
}

$ops=New-Fake;$script:Fake.failSave=1
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'service.SysMain'
Check (-not $r.ok -and $script:Fake.writes -eq 0) 'failed durable prewrite prevents settings change'
$ops=New-Fake;$script:Fake.failWrite='service.SysMain';$script:Fake.partial=$true
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'service.SysMain'
Check (-not $r.ok -and $script:Fake.journal.entries['service.SysMain'].phase -eq 'applyFailed') 'partial apply retains original'
$script:Fake.failWrite=''
$r=Invoke-SystemTuning -Ops $ops -Action Restore -Item 'service.SysMain'
Check ($r.ok -and $script:Fake.states['service.SysMain'].running -and $script:Fake.states['service.SysMain'].start -eq 2) 'partial apply restores both service fields'
$ops=New-Fake;$script:Fake.failSave=2
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'task.ScheduledDefrag'
Check (-not $r.ok -and -not $script:Fake.states['task.ScheduledDefrag'].enabled) 'failure after mutation reported'
$script:Fake.failSave=0
$r=Invoke-SystemTuning -Ops $ops -Action Restore -Item 'task.ScheduledDefrag'
Check ($r.ok -and $script:Fake.states['task.ScheduledDefrag'].enabled) 'pending/failed record recovers after save failure'

$ops=New-Fake;$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'service.SysMain'
$script:Fake.states['service.SysMain'].start=3
$r=Invoke-SystemTuning -Ops $ops -Action Restore -Item 'service.SysMain'
Check (-not $r.ok -and $script:Fake.states['service.SysMain'].start -eq 3) 'external drift is not overwritten'
Check ($script:Fake.journal.entries.Count -eq 1) 'conflicting original retained'
$ops=New-Fake;$r=Invoke-SystemTuning -Ops $ops -Action ApplyRecommended
Check $r.ok 'recommended apply'
$script:Fake.failWrite='service.DiagTrack'
$r=Invoke-SystemTuning -Ops $ops -Action RestoreAll
Check (-not $r.ok -and $script:Fake.journal.entries.Count -eq 1 -and $script:Fake.journal.entries.Contains('service.DiagTrack')) 'restore all retains only failed entry'
Check ($script:Fake.locks -eq $script:Fake.unlocks) 'lock released after partial failure'
$ops=New-Fake;$script:Fake.verifyFail='task.ScheduledDefrag'
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'task.ScheduledDefrag'
Check (-not $r.ok -and $script:Fake.journal.entries.Count -eq 1) 'readback mismatch retained'
$ops=New-Fake;$script:Fake.admin=$false
$r=Invoke-SystemTuning -Ops $ops -Action ApplyRecommended
Check (-not $r.ok -and $script:Fake.locks -eq 0 -and $script:Fake.saves -eq 0) 'machine mutations require admin'
$ops=New-Fake;$script:Fake.locked=$true
$r=Invoke-SystemTuning -Ops $ops -Action ApplyRecommended
Check (-not $r.ok -and $script:Fake.writes -eq 0) 'concurrent mutation refused'

foreach($now in @([datetime]'2026-12-31T12:00:00Z',[datetime]'2028-02-28T12:00:00Z')){
    foreach($days in @(1,35)){
        $ops=New-Fake;$script:Fake.now=$now
        $r=Invoke-SystemTuning -Ops $ops -Action PauseUpdates -PauseDays $days
        Check $r.ok 'finite pause configured'
        $item=@($r.items | Where-Object id -eq 'policy.UpdatePause')[0]
        Check ($item.requestedUntil -eq $now.ToUniversalTime().Date.AddDays($days).ToString('yyyy-MM-dd')) 'requested expiry across year or leap day'
        Check ($item.displayState -eq 'configured' -and $item.observedDisplayState -eq 'resumed') 'configured is distinct from observed'
        $script:Fake.now=$now.AddDays($days+1)
        $r=Invoke-SystemTuning -Ops $ops
        Check (@($r.items | Where-Object id -eq 'policy.UpdatePause')[0].displayState -eq 'notConfigured') 'expired pause not shown configured'
        $r=Invoke-SystemTuning -Ops $ops -Action ResumeUpdates
        Check ($r.ok -and -not $script:Fake.states['policy.UpdatePause'].quality.exists) 'resume restores absent policy'
    }
}
$ops=New-Fake
$script:Fake.states['policy.DriverUpdates'].exclude=@{exists=$true;kind='DWord';data=0}
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'policy.DriverUpdates'
$r=Invoke-SystemTuning -Ops $ops -Action Restore -Item 'policy.DriverUpdates'
Check ($r.ok -and $script:Fake.states['policy.DriverUpdates'].exclude.exists -and $script:Fake.states['policy.DriverUpdates'].exclude.data -eq 0) 'explicit zero policy restored'
$ops=New-Fake
$originalPause=@{exists=$true;kind='String';data='2026-12-25'}
$script:Fake.states['policy.UpdatePause'].quality=Copy-Value $originalPause
$r=Invoke-SystemTuning -Ops $ops -Action PauseUpdates -PauseDays 7
$r=Invoke-SystemTuning -Ops $ops -Action PauseUpdates -PauseDays 14
$r=Invoke-SystemTuning -Ops $ops -Action ResumeUpdates
Check ($r.ok -and $script:Fake.states['policy.UpdatePause'].quality.data -eq '2026-12-25') 'repeated pause preserves prior third-party date'
$ops=New-Fake;$script:Fake.failWrite='policy.UpdatePause';$script:Fake.partial=$true
$r=Invoke-SystemTuning -Ops $ops -Action PauseUpdates
Check (-not $r.ok -and $script:Fake.states['policy.UpdatePause'].quality.exists -and -not $script:Fake.states['policy.UpdatePause'].feature.exists) 'partial two-policy write recorded'
$script:Fake.failWrite=''
$r=Invoke-SystemTuning -Ops $ops -Action ResumeUpdates
Check ($r.ok -and -not $script:Fake.states['policy.UpdatePause'].quality.exists -and -not $script:Fake.states['policy.UpdatePause'].feature.exists) 'partial policy application restored'
$ops=New-Fake;$script:Fake.readUnsupported='policy.UpdatePause'
$r=Invoke-SystemTuning -Ops $ops -Action PauseUpdates
Check (-not $r.ok -and $script:Fake.writes -eq 0 -and $script:Fake.saves -eq 0) 'unsupported update environment not modified'
$ops=New-Fake;$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'service.SysMain'
$script:Fake.states['service.SysMain'].start='unexpected'
$r=Invoke-SystemTuning -Ops $ops
$item=@($r.items | Where-Object id -eq 'service.SysMain')[0]
Check ($item.displayState -eq 'unavailable' -and $null -eq $item.current -and -not $item.canRestore) 'unreadable current state never advertised as restorable'

$ops=New-Fake;$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'task.ScheduledDefrag'
$script:Fake.journal.entries['task.Security'] = $script:Fake.journal.entries['task.ScheduledDefrag']
$writes=$script:Fake.writes
$r=Invoke-SystemTuning -Ops $ops -Action RestoreAll
Check (-not $r.ok -and $script:Fake.writes -eq $writes) 'unknown journal item rejected'
$ops=New-Fake;$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'service.SysMain'
$script:Fake.journal.entries['service.SysMain'].target.start=2
$r=Invoke-SystemTuning -Ops $ops -Action RestoreAll
Check (-not $r.ok) 'tampered catalog target rejected'
$ops=New-Fake;$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item '..\Security'
Check (-not $r.ok -and $script:Fake.writes -eq 0) 'arbitrary caller target refused'

$legacy=@{schema='bc250-llmvram-debloat/1';host='FAKE-MACHINE';what_if_only=$false;services=@(@{name='SysMain';start_mode='Auto';state='Running'});tasks=@(@{path='\Microsoft\Windows\Defrag\';name='ScheduledDefrag';state='Ready'},@{path='\Microsoft\Windows\Windows Defender\';name='Windows Defender Scheduled Scan';state='Ready'});run_values=@()}
$ops=New-Fake;$script:Fake.states['task.ScheduledDefrag'].enabled=$false
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check (-not $r.ok -and $script:Fake.journal.entries.Contains('task.ScheduledDefrag')) 'complete legacy task imports despite incomplete service'
Check ($script:Fake.writes -eq 0 -and $script:Fake.journal.entries.Count -eq 1) 'legacy import changes no settings and excludes security task'
$original=$script:Fake.journal.entries['task.ScheduledDefrag'].original.enabled
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check (-not $r.ok -and $script:Fake.journal.entries['task.ScheduledDefrag'].original.enabled -eq $original) 'legacy import cannot overwrite original'
$r=Invoke-SystemTuning -Ops $ops -Action Restore -Item 'task.ScheduledDefrag'
Check ($r.ok -and $script:Fake.states['task.ScheduledDefrag'].enabled) 'legacy task restoration works'
$ops=New-Fake;$legacy.what_if_only=$true
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check (-not $r.ok -and $script:Fake.saves -eq 0) 'legacy dry-run rejected'
$legacy.what_if_only=$false;$legacy.host='OTHER-MACHINE'
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check (-not $r.ok -and $script:Fake.saves -eq 0) 'legacy foreign machine rejected'
$legacy.host='FAKE-MACHINE'
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check (-not $r.ok -and $script:Fake.saves -eq 0) 'legacy task with changed current state rejected'
$ops=New-Fake;$script:Fake.states['task.ScheduledDefrag'].enabled=$false
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy -DryRun
Check ($script:Fake.saves -eq 0 -and $script:Fake.writes -eq 0 -and $script:Fake.locks -eq 0 -and $r.importedCount -eq 1) 'legacy dry run previews without journal writes'
$legacy.services=@();$legacy.tasks=@();$legacy.run_values=@(@{key='HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run';name='TeamsMachineInstaller';kind='String';data='untrusted.exe --run'})
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check (-not $r.ok -and $r.rejectedCount -eq 1 -and $script:Fake.saves -eq 0) 'legacy cannot authorize a startup command even at a permitted value name'
$legacy.run_values=@();$legacy.services=@(@{name='SysMain';start_mode='Auto';state='Running';delayed=@{exists=$true;kind='DWord';data=1}})
$script:Fake.states['service.SysMain'].start=4;$script:Fake.states['service.SysMain'].running=$false
$r=Invoke-SystemTuning -Ops $ops -Action ImportLegacy -Legacy $legacy
Check ($r.ok -and $r.importedCount -eq 1 -and $script:Fake.writes -eq 0) 'complete safe service metadata can be adopted'

$ops=New-Fake -Scope User;$script:FakeScope='User';$script:Fake.admin=$false
$r=Invoke-SystemTuning -Ops $ops
Check ($r.ok -and $r.items.Count -eq 2 -and $r.scope -eq 'User') 'separate user catalog'
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'autostart.OneDrive'
Check $r.ok 'user apply requires no elevation'
$r=Invoke-SystemTuning -Ops $ops -Action RestoreAll
Check ($r.ok -and $script:Fake.states['autostart.OneDrive'].value.kind -eq 'ExpandString') 'user restore preserves raw ExpandString'
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'service.SysMain'
Check (-not $r.ok) 'machine targets unavailable in user scope'
$r=Invoke-SystemTuning -Ops $ops -Action PauseUpdates
Check (-not $r.ok) 'user scope cannot change update policies'
$r=Invoke-SystemTuning -Ops $ops -Action Apply -Item 'autostart.OneDrive'
$ops.Machine='DIFFERENT-USER'
$r=Invoke-SystemTuning -Ops $ops -Action RestoreAll
Check (-not $r.ok) 'foreign journal identity rejected'

# Parser gate includes the native adapter without loading it or reading any live setting.
foreach($file in @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps*')){
    $tokens=$null;$errors=$null
    [void][Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
    Check ($errors.Count -eq 0) ('PowerShell parser '+$file.Name)
}
[void][IO.Directory]::CreateDirectory($Out)
$script:FakeScope='Machine';$ops=New-Fake
$fixture=Invoke-SystemTuning -Ops $ops
[IO.File]::WriteAllText((Join-Path $Out 'list-machine.json'),($fixture | ConvertTo-Json -Depth 18),[Text.UTF8Encoding]::new($false))
$script:FakeScope='User';$ops=New-Fake -Scope User
$fixture=Invoke-SystemTuning -Ops $ops
[IO.File]::WriteAllText((Join-Path $Out 'list-user.json'),($fixture | ConvertTo-Json -Depth 18),[Text.UTF8Encoding]::new($false))
$report=[ordered]@{schema=1;ok=$true;checks=$script:Checks;nativeOperationsExecuted=0;utc=[datetime]::UtcNow.ToString('o')}
[IO.File]::WriteAllText((Join-Path $Out 'system-tuning-tests.json'),($report | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
Write-Output ('System tuning fake-system tests passed: '+$script:Checks)
