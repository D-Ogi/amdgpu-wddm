# BD-175: execute the installer's real post-binding setting body with a memory-only CLI.
# No worker, registry, SCM, process launch or reboot occurs.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\installer\common.ps1"
. "$PSScriptRoot\installer\state-lifecycle.ps1"
. "$PSScriptRoot\installer\state-reader.ps1"
$script:bd175Checks=0
function Assert-Bd175([bool]$Pass,[string]$Name) {
    $script:bd175Checks++
    if(-not $Pass){throw "BD-175 FAIL: $Name"}
}
$install=Join-Path $PSScriptRoot 'installer\install.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($install,[ref]$tokens,[ref]$errors)
Assert-Bd175 ($errors.Count -eq 0) 'installer parses'
$freshAssignments=@($ast.FindAll({param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -ceq '$freshCommandLineParameters'
},$true))
Assert-Bd175 ($freshAssignments.Count -eq 1) 'one fresh-argument selection in production'
$freshSelection=[scriptblock]::Create('param([int]$CuMode,[int]$DpmMaxMHz,[switch]$Repair) '+$freshAssignments[0].Right.Extent.Text)
Assert-Bd175 (@(& $freshSelection).Count -eq 0) 'resumed saved arguments are not fresh'
Assert-Bd175 (@(& $freshSelection -CuMode 0).Count -eq 0) 'CuMode zero sentinel is not a fresh CU choice'
$freshNames=@(& $freshSelection -CuMode 40 -DpmMaxMHz 1500 -Repair)
Assert-Bd175 ($freshNames.Count -eq 2 -and $freshNames -ccontains 'CuMode' -and
    $freshNames -ccontains 'DpmMaxMHz') 'only explicit CU and DPM choices are fresh'
$calls=@($ast.FindAll({param($node)
    $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Invoke-Change' -and
    $node.CommandElements.Count -ge 3 -and
    $node.CommandElements[1].Extent.Text -eq "'apply release setting policy through the validated shared-state resolver'"
},$true))
Assert-Bd175 ($calls.Count -eq 1) 'one production setting application after binding'
$body=$calls[0].CommandElements[2].ScriptBlock.GetScriptBlock()
$InstallRoot='P:\BC-250\scratch\fixture-never-executed'
$regDefaults=[pscustomobject]@{defaults=[pscustomobject]@{parameters=[pscustomobject]@{CuMode=24;DpmMaxMHz=2000}}}
$applied=[pscustomobject]@{parameters=[pscustomobject]@{CuMode=24;DpmMaxMHz=2000}}
$script:bd175Before=@{CuMode=40;CuDisableWgp=0xC4310;CuModeChosen=0xC431028;CuModeConfirmed=0xC431028;DpmMaxMHz=2000}
$script:bd175Live=@{};$script:bd175Writes=@();$script:bd175Snapshots=0;$script:bd175Denied=$false
function New-Bd175Read([string]$Name) {
    $present=$script:bd175Live.ContainsKey($Name)
    return [ordered]@{status=$(if($script:bd175Denied){'access'}elseif($present){'ok'}else{'absent'});
        type=4;bytes=4;value=$(if($present){$script:bd175Live[$Name]}else{$null})}
}
function Invoke-Native([string]$Exe,[string[]]$Arguments) {
    Assert-Bd175 ($Arguments[0] -ceq 'state') 'only the common state API is called'
    switch($Arguments[1]) {
        snapshot {
            $script:bd175Snapshots++
            $items=@(foreach($name in $script:bd175Before.Keys){
                $read=New-Bd175Read $name
                # CU records remain records, including the marker written for a
                # current explicit choice. They never become release defaults.
                $kind=if($name -in @('CuModeConfirmed','CuModeChosen','DpmClosedReason')){'record'}else{'setting'}
                $item=[ordered]@{path='';name=$name;kind=$kind;type=4}
                if($kind -eq 'record'){$item.record=$read}else{$item.override=$read;$item.effective=$read}
                [pscustomobject]$item
            })
            return @{code=0;text=(@{abi=1;error=0;items=$items}|ConvertTo-Json -Depth 8 -Compress)}
        }
        set {
            $name=$Arguments[2];$value=[uint32]$Arguments[3]
            $script:bd175Writes+=@($name);$script:bd175Live[$name]=$value
            return @{code=0;text='{"abi":1,"error":0}'}
        }
        clear {
            $script:bd175Writes+=@("clear:$($Arguments[2])")
            $script:bd175Live.Remove($Arguments[2]);return @{code=0;text='{"abi":1,"error":0}'}
        }
        get {return @{code=0;text=(@{abi=1;error=0;result=(New-Bd175Read $Arguments[2])}|ConvertTo-Json -Depth 6 -Compress)}}
        default {throw "unexpected command $($Arguments -join ' ')"}
    }
}
function Reset-Bd175 {
    $script:bd175Live=$script:bd175Before.Clone();$script:bd175Writes=@();$script:bd175Snapshots=0;$script:bd175Denied=$false
}
function Assert-Bd175Live([hashtable]$Expected) {
    Assert-Bd175 ($script:bd175Live.Count -eq $Expected.Count) 'no value is resurrected or deleted'
    foreach($name in $Expected.Keys){
        Assert-Bd175 ($script:bd175Live.ContainsKey($name) -and $script:bd175Live[$name] -eq $Expected[$name]) "current $name preserved"
    }
}
# The actual old snapshot is present in resumed state, as in rc4. It must never
# become the source of a post-binding write. Runtime records may change at boot.
$state=[pscustomobject]@{phase='driver-pending-restart';parameters_before_install=[pscustomobject]$script:bd175Before}
$instance='PCI\VEN_1002&DEV_13FE\FIXTURE';$driverSetSha='a'*64;$freshCommandLineParameters=@()
$freshRepair=$false;$script:ReopenClosures=$false
$compareItems=@(foreach($n in $script:bd175Before.Keys){[pscustomobject]@{name=$n;present=$true;value=$script:bd175Before[$n]}})
$state|Add-Member -NotePropertyName driver_settings_compare -NotePropertyValue ([pscustomobject]@{schema=1;device=$instance;inf_sha256=$driverSetSha;items=$compareItems})
$commandLineParameters=@{}
foreach($name in @('CuMode','CuDisableWgp','CuModeChosen','CuModeConfirmed','DpmMaxMHz')) {
    Reset-Bd175
    $script:bd175Live[$name]=switch($name){CuMode {24} CuDisableWgp {0x10} CuModeChosen {24} CuModeConfirmed {40} DpmMaxMHz {1300}}
    $expected=$script:bd175Live.Clone()
    & $body
    Assert-Bd175 ($script:bd175Snapshots -eq 1) 'continuation reads live state'
    Assert-Bd175 ($script:bd175Writes.Count -eq 0) "no stale write after changing $name"
    Assert-Bd175Live $expected
}
foreach($name in @('CuDisableWgp','CuModeChosen','CuModeConfirmed')) {
    Reset-Bd175;$script:bd175Live.Remove($name);$expected=$script:bd175Live.Clone()
    & $body
    Assert-Bd175 ($script:bd175Writes.Count -eq 0) "no restore of deleted $name"
    Assert-Bd175Live $expected
}
# The reported sequence: All(40) clears the mask and confirmation, and writes the
# new choice marker. A boot may then confirm 40. The saved -CuMode 40 is unchanged.
foreach($confirmed in @($false,$true)) {
    Reset-Bd175;$script:bd175Live.Remove('CuDisableWgp');$script:bd175Live.Remove('CuModeConfirmed')
    $script:bd175Live.CuModeChosen=40
    if($confirmed){$script:bd175Live.CuModeConfirmed=40}
    $expected=$script:bd175Live.Clone();$commandLineParameters=@{CuMode=40}
    & $body
    Assert-Bd175 ($script:bd175Writes.Count -eq 0) 'new CU choice/mask prevents replay of an old mode command'
    Assert-Bd175Live $expected
}
# Control: the previous raw snapshot replay would restore the stale CU mask.
$oldPlan=Get-RegistryDefaultPlan -Defaults @{CuMode=24;DpmMaxMHz=2000} -Previous $applied.parameters `
    -Current $script:bd175Before -After $expected -Restore @('CuDisableWgp','CuModeChosen','CuModeConfirmed')
Assert-Bd175 (@($oldPlan | Where-Object {$_.name -eq 'CuDisableWgp' -and $_.write -and $_.value -eq 0xC4310}).Count -eq 1) 'old replay reproduces the stale-mask write'
# Saved arguments and the old default migration are subject to the same rule.
foreach($name in @('CuMode','DpmMaxMHz')) {
    Reset-Bd175;$commandLineParameters=@{$name=$script:bd175Before[$name]}
    $script:bd175Live[$name]=if($name -eq 'CuMode'){24}else{1500};$expected=$script:bd175Live.Clone()
    & $body
    Assert-Bd175 ($script:bd175Writes.Count -eq 0) "saved command cannot undo later $name"
    Assert-Bd175Live $expected
    $freshCommandLineParameters=@($name)
    & $body
    $wantWrites=if($name -eq 'CuMode'){2}else{1}
    Assert-Bd175 ($script:bd175Writes.Count -eq $wantWrites -and $script:bd175Live[$name] -eq $commandLineParameters[$name]) "fresh explicit $name is still honored"
    $freshCommandLineParameters=@()
}
Reset-Bd175;$commandLineParameters=@{};$script:bd175Live.DpmMaxMHz=1500;$expected=$script:bd175Live.Clone()
& $body
Assert-Bd175 ($script:bd175Writes.Count -eq 0) 'later 1500 choice is not migrated to 2000'
Assert-Bd175Live $expected
foreach($name in @('CuMode','DpmMaxMHz')) {
    Reset-Bd175;$script:bd175Live.Remove($name);$commandLineParameters=@{$name=$script:bd175Before[$name]};$expected=$script:bd175Live.Clone()
    & $body
    Assert-Bd175 ($script:bd175Writes.Count -eq 0) "saved argument does not resurrect deleted $name"
    Assert-Bd175Live $expected
}
# Fresh installation: each absent value is still absent; apply the planned defaults and command.
Reset-Bd175;$savedItems=$state.driver_settings_compare.items
$state.driver_settings_compare.items=@(foreach($n in $script:bd175Before.Keys){[pscustomobject]@{name=$n;present=$false;value=0}})
$script:bd175Live=@{};$commandLineParameters=@{CuMode=40}
& $body
Assert-Bd175 ($script:bd175Live.CuMode -eq 40 -and $script:bd175Live.CuModeChosen -eq 40 -and $script:bd175Live.DpmMaxMHz -eq 2000 -and $script:bd175Writes.Count -eq 3) 'unchanged absence still applies defaults, pending command and choice record'
$state.driver_settings_compare.items=$savedItems
Reset-Bd175;$commandLineParameters=@{CuMode=40};$expected=$script:bd175Live.Clone()
& $body
Assert-Bd175 ($script:bd175Writes.Count -eq 2 -and $script:bd175Writes[0] -ceq 'CuMode' -and $script:bd175Writes[1] -ceq 'CuModeChosen') 'unchanged pending command writes through the state API with its choice record'
Assert-Bd175Live $expected
# Comparison identity and completeness fail before any setting write.
foreach($field in @('device','inf_sha256','schema')) {
    Reset-Bd175;$old=$state.driver_settings_compare.$field;$state.driver_settings_compare.$field='wrong';$failed=$false
    try {& $body}catch{$failed=$true}
    Assert-Bd175 ($failed -and $script:bd175Writes.Count -eq 0) "wrong comparison $field refuses"
    $state.driver_settings_compare.$field=$old
}
Reset-Bd175;$state.driver_settings_compare.items=@($savedItems|Where-Object name -ne 'DpmMaxMHz');$failed=$false
try {& $body}catch{$failed=$true}
Assert-Bd175 ($failed -and $script:bd175Writes.Count -eq 0) 'incomplete comparison refuses before writes'
$state.driver_settings_compare.items=$savedItems
# Pre-stage capture is saved once and reused on retry, even if a GUI write occurs.
$capture=[pscustomobject]@{};$script:captureReads=0;$script:captureSaves=0
$captureRunner={param($exe,$a)
    Assert-Bd175 ($a[0] -eq '--settings-snapshot' -and $a[1] -eq $instance) 'read-only comparison command'
    $script:captureReads++;return @{code=0;text=(@{abi=1;error=0;items=$compareItems}|ConvertTo-Json -Depth 5 -Compress)}
}
$captureSave={param($s)$script:captureSaves++}
Save-DriverSettingsComparison $capture fixture.exe $instance $driverSetSha $captureSave $captureRunner
Save-DriverSettingsComparison $capture fixture.exe $instance $driverSetSha $captureSave $captureRunner
Assert-Bd175 ($script:captureReads -eq 1 -and $script:captureSaves -eq 1) 'retry does not move the pre-stage observation'
$capture.driver_settings_compare=$null;$capture|Add-Member -NotePropertyName driver_state -NotePropertyValue ([pscustomobject]@{phase='prepared'})
$failed=$false;try {Save-DriverSettingsComparison $capture fixture.exe $instance $driverSetSha $captureSave $captureRunner}catch{$failed=$true}
Assert-Bd175 ($failed -and $script:captureReads -eq 1) 'missing comparison after preparation refuses instead of recapturing'
# Repair reopens only the closure it observed; a newer closure is preserved.
$originalBefore=$script:bd175Before.Clone()
$script:bd175Before.DpmMode=0;$script:bd175Before.DpmClosedReason=3
$regDefaults.defaults.parameters|Add-Member -NotePropertyName DpmMode -NotePropertyValue 1
$applied.parameters|Add-Member -NotePropertyName DpmMode -NotePropertyValue 1
$state.driver_settings_compare.items=@(foreach($n in $script:bd175Before.Keys){[pscustomobject]@{name=$n;present=$true;value=$script:bd175Before[$n]}})
$commandLineParameters=@{};$script:ReopenClosures=$true
Reset-Bd175
& $body
Assert-Bd175 ($script:bd175Live.DpmMode -eq 1 -and -not $script:bd175Live.ContainsKey('DpmClosedReason')) 'unchanged closure reopens and its record is cleared through the state API'
Reset-Bd175;$script:bd175Live.DpmClosedReason=4;$expected=$script:bd175Live.Clone()
& $body
Assert-Bd175 ($script:bd175Writes.Count -eq 0) 'saved repair cannot clear a newer closure'
Assert-Bd175Live $expected
$freshRepair=$true
& $body
Assert-Bd175 ($script:bd175Live.DpmMode -eq 1 -and -not $script:bd175Live.ContainsKey('DpmClosedReason')) 'fresh repair intentionally reopens the current closure'
$freshRepair=$false;$script:ReopenClosures=$false
$script:bd175Before=$originalBefore;$state.driver_settings_compare.items=$savedItems
$regDefaults.defaults.parameters.PSObject.Properties.Remove('DpmMode');$applied.parameters.PSObject.Properties.Remove('DpmMode')
# No current reading means no invented state and no writes.
Reset-Bd175;$script:bd175Denied=$true;$failed=$false
try {& $body}catch{$failed=$true}
Assert-Bd175 ($failed -and $script:bd175Writes.Count -eq 0) 'unreadable live state refuses before writing'
"continuation settings: $script:bd175Checks checks PASS"
