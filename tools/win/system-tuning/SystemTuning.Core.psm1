# The core has no operating-system side effects. Tests inject every native operation.
Set-StrictMode -Version 2

function Get-TuningCatalog {
    param([ValidateSet('Machine','User')][string]$Scope='Machine')
    if($Scope -eq 'User'){
        @(
            @{id='autostart.OneDrive';kind='autostart';target='SOFTWARE\Microsoft\Windows\CurrentVersion\Run';value='OneDrive';label='OneDrive autostart';recommended=$false;note='Stops automatic OneDrive launch for this user. File synchronization then requires a manual launch.'},
            @{id='autostart.Teams';kind='autostart';target='SOFTWARE\Microsoft\Windows\CurrentVersion\Run';value='com.squirrel.Teams.Teams';label='Classic Teams autostart';recommended=$false;note='Stops automatic classic Teams launch for this user.'}
        ) | ForEach-Object { [pscustomobject]$_ }
        return
    }
    @(
        @{ id='service.SysMain'; kind='service'; target='SysMain'; label='SysMain preloading'; recommended=$false; note='May increase application startup time.' },
        @{ id='service.WSearch'; kind='service'; target='WSearch'; label='Windows Search indexing'; recommended=$false; note='Indexed file and mail searches may be slower.' },
        @{ id='service.DiagTrack'; kind='service'; target='DiagTrack'; label='Diagnostic telemetry service'; recommended=$true; note='Stops this diagnostic collection service.' },
        @{ id='service.MapsBroker'; kind='service'; target='MapsBroker'; label='Downloaded maps service'; recommended=$true; note='Offline maps will not update automatically.' },
        @{ id='task.ScheduledDefrag'; kind='task'; target='\Microsoft\Windows\Defrag\ScheduledDefrag'; label='Scheduled drive optimization'; recommended=$false; note='Disables automatic optimization, including scheduled SSD maintenance.' },
        @{ id='task.CompatibilityAppraiser'; kind='task'; target='\Microsoft\Windows\Application Experience\Microsoft Compatibility Appraiser'; label='Compatibility telemetry task'; recommended=$false; note='Disables application compatibility telemetry collection.' },
        @{ id='task.ProgramDataUpdater'; kind='task'; target='\Microsoft\Windows\Application Experience\ProgramDataUpdater'; label='Program telemetry task'; recommended=$true; note='Disables this program telemetry collection task.' },
        @{ id='task.Consolidator'; kind='task'; target='\Microsoft\Windows\Customer Experience Improvement Program\Consolidator'; label='Experience telemetry consolidation'; recommended=$true; note='Disables this customer experience task.' },
        @{ id='task.UsbCeip'; kind='task'; target='\Microsoft\Windows\Customer Experience Improvement Program\UsbCeip'; label='USB experience telemetry'; recommended=$true; note='Disables this USB telemetry task, not USB devices.' },
        @{ id='autostart.TeamsMachineInstaller'; kind='autostart'; target='SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run'; value='TeamsMachineInstaller'; label='Teams machine installer autostart'; recommended=$false; note='Removes only the machine-wide legacy Teams installer launch entry.' },
        @{ id='policy.DriverUpdates'; kind='driverPolicy'; target='SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'; label='Exclude drivers from quality updates'; recommended=$false; note='Applies to all driver offers in quality updates. Feature and critical OS drivers remain possible.' },
        @{ id='policy.UpdatePause'; kind='pausePolicy'; target='SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'; label='Pause Windows updates'; recommended=$false; note='Finite policy pause. Windows applies policy asynchronously. Security updates are delayed too.' }
    ) | ForEach-Object { [pscustomobject]$_ }
}

function ConvertTo-TuningMap($Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Collections.IDictionary]) {
        $map=@{}; foreach ($key in $Value.Keys) { $map[$key]=ConvertTo-TuningMap $Value[$key] }; return $map
    }
    if ($Value -is [pscustomobject]) {
        $map=@{}; foreach ($p in $Value.PSObject.Properties) { $map[$p.Name]=ConvertTo-TuningMap $p.Value }; return $map
    }
    if ($Value -is [array]) { return ,@($Value | ForEach-Object { ConvertTo-TuningMap $_ }) }
    return $Value
}

function Assert-Keys($Map, [string[]]$Keys) {
    if ($Map -isnot [System.Collections.IDictionary] -or $Map.Count -ne $Keys.Count) { throw 'Invalid record shape.' }
    foreach ($key in $Map.Keys) { if ($Keys -cnotcontains [string]$key) { throw 'Unknown record field.' } }
}

function Assert-RegistryValue($Value, [string]$Kind) {
    Assert-Keys $Value @('exists','kind','data')
    if ($Value.exists -isnot [bool] -or $Value.kind -cne $Kind) { throw 'Invalid registry value record.' }
    if (-not $Value.exists) { if ($null -ne $Value.data) { throw 'Absent registry value has data.' }; return }
    if ($Kind -eq 'DWord') {
        if ($Value.data -isnot [int] -and $Value.data -isnot [long]) { throw 'Invalid DWORD type.' }
        if ($Value.data -lt 0 -or $Value.data -gt 1) { throw 'Unsupported DWORD value.' }
    } elseif ($Value.data -isnot [string] -or $Value.data.Length -gt 32767 -or $Value.data.Contains([char]0)) {
        throw 'Invalid string value.'
    }
}

function Assert-TuningState($Descriptor, $State) {
    switch ($Descriptor.kind) {
        service {
            Assert-Keys $State @('present','start','running','delayed')
            if ($State.present -isnot [bool] -or $State.running -isnot [bool] -or $State.start -isnot [int] -or $State.start -notin @(2,3,4)) { throw 'Invalid service state.' }
            Assert-RegistryValue $State.delayed 'DWord'
        }
        task {
            Assert-Keys $State @('present','enabled')
            if ($State.present -isnot [bool] -or $State.enabled -isnot [bool]) { throw 'Invalid task state.' }
        }
        autostart {
            Assert-Keys $State @('present','value')
            if ($State.present -isnot [bool] -or $State.value.kind -cnotin @('String','ExpandString')) { throw 'Invalid autostart state.' }
            Assert-RegistryValue $State.value $State.value.kind
            if ($State.present -ne $State.value.exists) { throw 'Invalid autostart presence.' }
        }
        driverPolicy {
            Assert-Keys $State @('present','exclude')
            if ($State.present -isnot [bool]) { throw 'Invalid policy state.' }
            Assert-RegistryValue $State.exclude 'DWord'
        }
        pausePolicy {
            Assert-Keys $State @('present','quality','feature')
            if ($State.present -isnot [bool]) { throw 'Invalid policy state.' }
            foreach ($name in @('quality','feature')) {
                Assert-RegistryValue $State[$name] 'String'
                if ($State[$name].exists -and $State[$name].data -ne '') {
                    $parsed=[datetime]::MinValue
                    if (-not [datetime]::TryParseExact($State[$name].data,'yyyy-MM-dd',[cultureinfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$parsed)) { throw 'Invalid pause date.' }
                }
            }
        }
        default { throw 'Unknown catalog kind.' }
    }
}

function Test-EqualState($A, $B) {
    if ($null -eq $A -or $null -eq $B) { return ($null -eq $A -and $null -eq $B) }
    if ($A -is [System.Collections.IDictionary]) {
        if ($B -isnot [System.Collections.IDictionary] -or $A.Count -ne $B.Count) { return $false }
        foreach ($key in $A.Keys) { if (-not $B.Contains($key) -or -not (Test-EqualState $A[$key] $B[$key])) { return $false } }
        return $true
    }
    return ($A.GetType() -eq $B.GetType() -and $A -ceq $B)
}

function Test-PartialState($Current, $Original, $Target) {
    # An interrupted compound change can contain either original or intended leaves.
    if (Test-EqualState $Current $Original) { return $true }
    if (Test-EqualState $Current $Target) { return $true }
    if ($Current -isnot [System.Collections.IDictionary] -or $Original -isnot [System.Collections.IDictionary] -or $Target -isnot [System.Collections.IDictionary]) { return $false }
    if ($Current.Count -ne $Original.Count -or $Current.Count -ne $Target.Count) { return $false }
    foreach ($key in $Current.Keys) {
        if (-not $Original.Contains($key) -or -not $Target.Contains($key) -or -not (Test-PartialState $Current[$key] $Original[$key] $Target[$key])) { return $false }
    }
    return $true
}

function New-TuningTarget($Descriptor, $Current, [datetime]$Now, [int]$PauseDays) {
    $target=ConvertTo-TuningMap $Current
    switch ($Descriptor.kind) {
        service { $target.start=4; $target.running=$false }
        task { $target.enabled=$false }
        autostart { $target.present=$false; $target.value=@{exists=$false;kind='String';data=$null} }
        driverPolicy { $target.exclude=@{exists=$true;kind='DWord';data=1} }
        pausePolicy {
            $start=$Now.ToUniversalTime().Date.AddDays($PauseDays-35).ToString('yyyy-MM-dd')
            $target.quality=@{exists=$true;kind='String';data=$start}
            $target.feature=@{exists=$true;kind='String';data=$start}
        }
    }
    return $target
}

function Assert-TuningJournal($Journal, $Catalog, [string]$Machine) {
    Assert-Keys $Journal @('schema','machine','entries')
    if ($Journal.schema -isnot [int] -or $Journal.schema -ne 1 -or $Journal.machine -cne $Machine -or $Journal.entries -isnot [System.Collections.IDictionary]) { throw 'Invalid journal identity or schema.' }
    if ($Journal.entries.Count -gt $Catalog.Count) { throw 'Journal has too many entries.' }
    foreach ($id in $Journal.entries.Keys) {
        $d=@($Catalog | Where-Object { $_.id -ceq $id })
        if ($d.Count -ne 1) { throw 'Journal contains an unknown item.' }
        $e=$Journal.entries[$id]
        Assert-Keys $e @('original','target','phase','error','updatedUtc','source')
        if ($e.phase -cnotin @('applying','applied','applyFailed','restoring','restoreFailed') -or $e.error -isnot [string] -or $e.error.Length -gt 4096 -or $e.source -cnotin @('native','legacy')) { throw 'Invalid journal phase.' }
        if ($e.updatedUtc -isnot [string]) { throw 'Invalid journal timestamp.' }
        Assert-TuningState $d[0] $e.original
        Assert-TuningState $d[0] $e.target
        # The target must be a permitted operation, not a second arbitrary original.
        $expected=New-TuningTarget $d[0] $e.original ([datetime]::UtcNow) 1
        if ($d[0].kind -eq 'pausePolicy') {
            $expected.quality=$e.target.quality; $expected.feature=$e.target.quality
            if (-not $e.target.quality.exists -or $e.target.quality.data -eq '') { throw 'Invalid recorded pause target.' }
        }
        if (-not (Test-EqualState $expected $e.target)) { throw 'Journal target is outside the catalog operation.' }
    }
}

function Get-TuningItems($Ops, $Catalog, $Journal) {
    foreach ($d in $Catalog) {
        $managed=$Journal.entries.Contains($d.id); $note=$d.note; $current=$null; $available=$false; $present=$false
        $observed=$null;$requestedUntil=$null;$display='unavailable';$observedDisplay='unknown'
        try {
            $read=& $Ops.Read $d; $current=ConvertTo-TuningMap $read.state
            Assert-TuningState $d $current
            $present=$current.present; $available=[bool]$read.supported
            if ($read.note) { $note+=' '+$read.note }
            $display=if($present){'enabled'}else{'absent'}
            switch($d.kind){
                service { if($present){$display=if($current.start -eq 4 -and -not $current.running){'disabled'}elseif($current.start -ne 4){'enabled'}else{'mixed'}} }
                task { if($present -and -not $current.enabled){$display='disabled'} }
                autostart { if(-not $current.present -and $managed){$display='disabled'} }
                driverPolicy { $display=if($current.exclude.exists -and $current.exclude.data -eq 1){'configured'}else{'notConfigured'} }
                pausePolicy {
                    $dates=@();foreach($v in @($current.quality,$current.feature)){if($v.exists -and $v.data){$dates+=([datetime]::ParseExact($v.data,'yyyy-MM-dd',[cultureinfo]::InvariantCulture)).AddDays(35)}}
                    $display='notConfigured'
                    if($dates.Count -eq 2 -and $dates[0] -eq $dates[1]){$requestedUntil=$dates[0].ToString('yyyy-MM-dd');if($dates[0] -gt (& $Ops.Now).ToUniversalTime().Date){$display='configured'}}
                    elseif($dates.Count -gt 0){$display='mixed'}
                    if($read.Contains('observed')){$observed=$read.observed}
                    if($null -ne $observed -and $null -ne $observed.qualityStatus -and $null -ne $observed.featureStatus){
                        $q=$observed.qualityStatus -eq 1;$f=$observed.featureStatus -eq 1
                        $observedDisplay=if($q -and $f){'paused'}elseif(-not $q -and -not $f){'resumed'}else{'mixed'}
                    }
                }
            }
        } catch { $note+=' Cannot read: '+$_.Exception.Message;$current=$null;$present=$false;$available=$false;$display='unavailable' }
        $phase=if($managed){$Journal.entries[$d.id].phase}else{'unmanaged'}
        [pscustomobject]@{id=$d.id;kind=$d.kind;label=$d.label;recommended=$d.recommended;present=$present;current=$current;managed=$managed;canApply=($available -and $present);canRestore=($managed -and $null -ne $current);note=$note;state=$phase;displayState=$display;requestedUntil=$requestedUntil;observed=$observed;observedDisplayState=$observedDisplay}
    }
}

function Import-TuningLegacy($Legacy, $Ops, $Catalog, $Journal, [bool]$DryRun) {
    if ($Legacy -isnot [System.Collections.IDictionary] -or $Legacy.schema -cne 'bc250-llmvram-debloat/1' -or $Legacy.what_if_only -isnot [bool] -or $Legacy.what_if_only) { throw 'Unsupported or dry-run legacy record.' }
    if (-not $Legacy.Contains('host') -or $Legacy.host -ine $Ops.Machine) { throw 'Legacy record belongs to a different machine.' }
    foreach ($section in @('services','tasks','run_values')) {
        if (-not $Legacy.Contains($section) -or $Legacy[$section] -isnot [array]) { throw 'Incomplete legacy record.' }
    }
    $results=@(); $seen=@{}
    foreach ($section in @('services','tasks','run_values')) {
        foreach ($item in $Legacy[$section]) {
            $d=$null; $original=$null
            switch ($section) {
                services { $d=@($Catalog | Where-Object { $_.kind -eq 'service' -and $_.target -ceq $item.name }) }
                tasks { $d=@($Catalog | Where-Object { $_.kind -eq 'task' -and $_.target -ceq ($item.path+$item.name) }) }
                run_values { $d=@($Catalog | Where-Object { $_.kind -eq 'autostart' -and ('HKLM:\'+$_.target) -ceq $item.key -and $_.value -ceq $item.name }) }
            }
            if ($d.Count -ne 1) { $results+=@{id='';ok=$false;status='excluded';error='Legacy target is outside the fixed catalog.'}; continue }
            $d=$d[0]
            try {
                if ($seen.Contains($d.id)) { throw 'Duplicate legacy item.' }; $seen[$d.id]=$true
                if ($Journal.entries.Contains($d.id)) { throw 'An original state is already recorded.' }
                switch ($section) {
                    services {
                        # Older lab records omit this field. Do not invent the missing original.
                        if (-not $item.Contains('delayed')) { throw 'Legacy service lacks original DelayedAutoStart. Import refused.' }
                        $start=switch($item.start_mode){'Auto'{2};'Manual'{3};'Disabled'{4};default{throw 'Invalid legacy startup mode.'}}
                        if ($item.state -cnotin @('Running','Stopped')) { throw 'Invalid legacy service state.' }
                        $original=@{present=$true;start=$start;running=($item.state -ceq 'Running');delayed=$item.delayed}
                    }
                    tasks {
                        if ($item.state -cnotin @('Disabled','Ready','Running','Queued')) { throw 'Unknown legacy task state.' }
                        $original=@{present=$true;enabled=($item.state -cne 'Disabled')}
                    }
                    run_values { throw 'Legacy autostart commands are not authenticated. Import refused.' }
                }
                Assert-TuningState $d $original
                $target=New-TuningTarget $d $original (& $Ops.Now) 1
                $current=ConvertTo-TuningMap (& $Ops.Read $d).state
                Assert-TuningState $d $current
                if (-not (Test-EqualState $current $target)) { throw 'Current state does not match the legacy change.' }
                if (-not $DryRun) {
                    $Journal.entries[$d.id]=@{original=$original;target=$target;phase='applied';error='';updatedUtc=(& $Ops.Now).ToUniversalTime().ToString('o');source='legacy'}
                    & $Ops.Save $Journal
                }
                $results+=@{id=$d.id;ok=$true;status=$(if($DryRun){'wouldImport'}else{'imported'});error=''}
            } catch { $results+=@{id=$d.id;ok=$false;status='rejected';error=$_.Exception.Message} }
        }
    }
    return $results
}

function Invoke-SystemTuning {
    [CmdletBinding()]
    param([Parameter(Mandatory)][hashtable]$Ops,
        [ValidateSet('List','Apply','Restore','RestoreAll','ApplyRecommended','PauseUpdates','ResumeUpdates','ImportLegacy')][string]$Action='List',
        [string]$Item='', [ValidateRange(1,35)][int]$PauseDays=7, [switch]$DryRun, $Legacy=$null)
    $scope=if($Ops.Contains('Scope')){$Ops.Scope}else{'Machine'}
    $result=[ordered]@{schema=1;ok=$false;error='';action=$Action;scope=$scope;dryRun=[bool]$DryRun;items=@();results=@()}
    $lock=$null; $catalog=@(Get-TuningCatalog -Scope $scope)
    try {
        if($scope -eq 'User' -and $Action -in @('PauseUpdates','ResumeUpdates','ImportLegacy')){throw 'This action requires Machine scope.'}
        if ($Action -ne 'List' -and -not $DryRun) {
            if (-not (& $Ops.IsAdmin)) { throw 'Administrator rights are required.' }
            $lock=& $Ops.Acquire
        }
        $journal=ConvertTo-TuningMap (& $Ops.Load)
        if ($null -eq $journal) { $journal=@{schema=1;machine=$Ops.Machine;entries=@{}} }
        Assert-TuningJournal $journal $catalog $Ops.Machine
        if ($Action -eq 'ImportLegacy') {
            $result.results=@(Import-TuningLegacy (ConvertTo-TuningMap $Legacy) $Ops $catalog $journal ([bool]$DryRun))
            $result.importedCount=@($result.results | Where-Object {$_.status -in @('imported','wouldImport')}).Count
            $result.rejectedCount=@($result.results | Where-Object {$_.status -eq 'rejected'}).Count
            $result.excludedCount=@($result.results | Where-Object {$_.status -eq 'excluded'}).Count
            $result.ok=(@($result.results | Where-Object { $_.status -eq 'rejected' }).Count -eq 0)
        } else {
            $ids=@(); $restore=$Action -in @('Restore','RestoreAll','ResumeUpdates')
            switch($Action) {
                Apply { $ids=@($Item) }
                Restore { $ids=@($Item) }
                RestoreAll { $ids=@($catalog | Where-Object { $journal.entries.Contains($_.id) } | ForEach-Object {$_.id}) }
                ApplyRecommended { $ids=@($catalog | Where-Object {$_.recommended} | ForEach-Object {$_.id}) }
                PauseUpdates { $ids=@('policy.UpdatePause') }
                ResumeUpdates { $ids=@('policy.UpdatePause') }
            }
            foreach ($id in $ids) {
                $r=@{id=$id;ok=$false;status='failed';error=''}
                try {
                    $matches=@($catalog | Where-Object {$_.id -ceq $id})
                    if ($matches.Count -ne 1) { throw 'Unknown catalog item.' }; $d=$matches[0]
                    if ($Action -eq 'Apply' -and $d.kind -eq 'pausePolicy') { throw 'Use PauseUpdates with a duration.' }
                    $entry=$journal.entries[$id]
                    if ($restore -and $null -eq $entry) { $r.ok=$true; $r.status='unmanaged'; continue }
                    $read=& $Ops.Read $d; $current=ConvertTo-TuningMap $read.state; Assert-TuningState $d $current
                    $removedAutostart=($d.kind -eq 'autostart' -and $null -ne $entry -and -not $current.present)
                    if (-not $restore -and (-not $read.supported -or (-not $current.present -and -not $removedAutostart))) {
                        if ($Action -eq 'ApplyRecommended' -and -not $current.present) { $r.ok=$true;$r.status='absent';continue }
                        throw ('Item is absent or unsupported. '+$read.note)
                    }
                    if ($null -ne $entry) {
                        $safe=if($entry.phase -eq 'applied'){Test-EqualState $current $entry.target}else{Test-PartialState $current $entry.original $entry.target}
                        if (-not $safe -and -not ($restore -and (Test-EqualState $current $entry.original))) { throw 'Conflict: current settings changed outside this tool.' }
                    }
                    $target=if($restore){$entry.original}else{New-TuningTarget $d $current (& $Ops.Now) $PauseDays}
                    if ($DryRun) { $r.ok=$true;$r.status=$(if($restore){'wouldRestore'}else{'wouldApply'});continue }
                    if ($null -eq $entry) { $entry=@{original=$current;target=$target;phase='applying';error='';updatedUtc='';source='native'}; $journal.entries[$id]=$entry }
                    if (-not $restore) { $entry.target=$target }
                    $entry.phase=if($restore){'restoring'}else{'applying'}
                    $entry.error='';$entry.updatedUtc=(& $Ops.Now).ToUniversalTime().ToString('o')
                    & $Ops.Save $journal
                    try {
                        if (-not (Test-EqualState $current $target)) { & $Ops.Write $d $target }
                        $verify=ConvertTo-TuningMap (& $Ops.Read $d).state
                        Assert-TuningState $d $verify
                        if (-not (Test-EqualState $verify $target)) { throw 'The operating system did not retain the requested setting.' }
                        if ($restore) { $journal.entries.Remove($id) } else { $entry.phase='applied' }
                        & $Ops.Save $journal
                        $r.ok=$true;$r.status=if($restore){'restored'}else{'applied'}
                    } catch {
                        # The durable pre-change entry remains even if the next save also fails.
                        $journal.entries[$id]=$entry
                        $entry.phase=if($restore){'restoreFailed'}else{'applyFailed'}
                        $entry.error=$_.Exception.Message.Substring(0,[Math]::Min(4096,$_.Exception.Message.Length))
                        try { & $Ops.Save $journal } catch { }
                        throw
                    }
                } catch { $r.error=$_.Exception.Message }
                finally { $result.results+=,$r }
            }
            $result.ok=(@($result.results | Where-Object {-not $_.ok}).Count -eq 0)
        }
        $result.items=@(Get-TuningItems $Ops $catalog $journal)
        if (-not $result.ok) { $result.error='One or more items could not be changed. See results.' }
    } catch { $result.error=$_.Exception.Message; $result.ok=$false }
    finally { if ($null -ne $lock) { & $Ops.Release $lock } }
    return [pscustomobject]$result
}

Export-ModuleMember -Function Get-TuningCatalog,Invoke-SystemTuning,ConvertTo-TuningMap
