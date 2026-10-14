# CPU-only orchestration tests. Every native call and durable installer-state save
# is an in-memory adapter. No worker, SCM, HKLM, task or display operation runs.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\installer\common.ps1"
. "$PSScriptRoot\installer\state-lifecycle.ps1"
. "$PSScriptRoot\installer\state-reader.ps1"
$script:checks=0
function Check([bool]$Pass,[string]$Name){$script:checks++;if(-not $Pass){throw "FAIL: $Name"}}
function Refuses([scriptblock]$Body,[string]$Name){$failed=$false;try {& $Body}catch{$failed=$true};Check $failed $Name}
$script:events=New-Object Collections.Generic.List[string]
$save={param($s) $script:events.Add("save:$($s.driver_state.phase)")}
$script:failWorker=$false
$run={param($exe,$arguments)
    $script:events.Add("worker:$($arguments[0])")
    if($script:failWorker){return @{code=5;text='denied'}}
    return @{code=0;text='{"abi":1,"error":0}'}
}
$s=[pscustomobject]@{};$sha='A'*64;$device='PCI\VEN_1002&DEV_13FE\FIXTURE'
$r=Start-DriverStateTransaction -State $s -Worker 'fixture.exe' -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $save -Runner $run
Check ($script:events[0] -eq 'save:prepared' -and $script:events[1] -eq 'worker:prepare') 'receipt precedes worker mutation'
$tx=$r.transaction
[void](Start-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $save -Runner $run)
Check ($s.driver_state.transaction -ceq $tx) 'interrupted preparation retries the same identity'
Refuses {Start-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.103' -Device $device -InfSha256 $sha -Save $save -Runner $run} 'different package cannot replace pending transaction'
Refuses {Complete-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -InfSha256 ('B'*64) -Save $save -Runner $run} 'different INF cannot acknowledge'
Refuses {Save-DriverStateBeforeRemoval -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -Save $save -Runner $run} 'uninstall blocks while migration pending'
$script:failWorker=$true
Refuses {Complete-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $save -Runner $run} 'failed native ack is retained'
Check ($s.driver_state.phase -eq 'prepared') 'native failure never marks complete'
$script:failWorker=$false
Complete-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $save -Runner $run
Check ($s.driver_state.phase -eq 'acknowledged') 'exact generation acknowledgment finishes'
Complete-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $save -Runner $run
Check ($script:events[$script:events.Count-2] -eq 'worker:ack') 'ack retry still checks protected durable state'
$script:failWorker=$true
Refuses {Save-DriverStateBeforeRemoval -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -Save $save -Runner $run} 'failed stop/export blocks uninstall'
Check ($s.driver_state_removal.phase -eq 'capture') 'failed final capture is not archived'
$script:failWorker=$false
Save-DriverStateBeforeRemoval -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -Save $save -Runner $run
Check ($s.driver_state_removal.phase -eq 'archived') 'successful final capture permits removal'
[void](Start-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.101' -Device $device -InfSha256 ('C'*64) -Save $save -Runner $run)
Check ($s.driver_state.transaction -cne $tx) 'same-schema rollback captures a new current generation'
$badReply={param($exe,$arguments) @{code=0;text='{"abi":1}'}}
Refuses {Invoke-DriverStateWorker fixture.exe prepare $tx '0.7.216.102' $device -Runner $badReply} 'zero process exit with missing durable ack is failure'
$badReply={param($exe,$arguments) @{code=0;text='{"abi":99,"error":0}'}}
Refuses {Invoke-DriverStateWorker fixture.exe prepare $tx '0.7.216.102' $device -Runner $badReply} 'ABI mismatch is failure'

function Item([string]$Name,[string]$Kind,[string]$Status,$Value,[string]$Path='') {
    $read=[pscustomobject]@{status=$Status;os_error=0;source='override';type=4;bytes=4;value=$Value}
    if($Kind -eq 'setting'){return [pscustomobject]@{path=$Path;name=$Name;kind=$Kind;override=$read;effective=$read}}
    return [pscustomobject]@{path=$Path;name=$Name;kind=$Kind;record=$read}
}
$script:items=@((Item CpuTune setting ok 0),(Item CuMode setting ok 24),(Item UnconfirmedStarts record ok 1))
$script:writes=New-Object Collections.Generic.List[string]
$stateRun={param($exe,$arguments)
    if($arguments[1] -eq 'snapshot'){return @{code=0;text=([ordered]@{abi=1;error=0;items=$script:items}|ConvertTo-Json -Depth 8 -Compress)}}
    if($arguments[1] -eq 'get'){return @{code=0;text=(@{abi=1;error=0;result=@{status='ok';type=4;bytes=4;value=40}}|ConvertTo-Json -Depth 4 -Compress)}}
    $script:writes.Add(($arguments -join ' '));return @{code=0;text='{"abi":1,"error":0}'}
}
Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1;CuMode=24;UnconfirmedStarts=0} -Previous @{CpuTune=1;CuMode=40} -Runner $stateRun
Check ($script:writes.Count -eq 0) 'explicit CpuTune off and runtime record survive release policy'
Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1;CuMode=24} -Previous @{CpuTune=1;CuMode=40} -Explicit @{CuMode=40} -Runner $stateRun
Check ($script:writes.Count -eq 1 -and $script:writes[0] -eq 'state set CuMode 40') 'explicit choice goes through common writer'
$script:items[0].override.status='access'
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1} -Previous @{} -Runner $stateRun} 'denied override never becomes default'
$script:items[0].override.status='type'
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1} -Previous @{} -Runner $stateRun} 'malformed override never becomes default'
$script:items[0].override.status='ok';$script:items[2].record.status='access'
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1} -Previous @{} -Runner $stateRun} 'denied closure never becomes absent'
$script:items[2].record.status='ok';$script:items[2].record.bytes=3
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1} -Previous @{} -Runner $stateRun} 'short closure never becomes absent'
$script:items[2].record.bytes=4
$script:items[2].record.type=1
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1} -Previous @{} -Runner $stateRun} 'wrong-type closure never becomes absent'
$script:items[2].record.type=4
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1} -Previous @{} -Runner {param($x,$a) @{code=0;text='{"abi":1,"error":0}'}}} 'missing snapshot items refused'
$script:batchWrites=0;$script:batchReadbacks=0;$script:lastBatchValue=0
$interrupted={param($exe,$arguments)
    switch($arguments[1]) {
        snapshot {return @{code=0;text=(@{abi=1;error=0;items=@((Item CpuTune setting ok 0),(Item CuMode setting ok 24))}|ConvertTo-Json -Depth 7 -Compress)}}
        set {$script:batchWrites++;if($script:batchWrites -eq 2){return @{code=5;text='interrupted'}};$script:lastBatchValue=[uint32]$arguments[3];return @{code=0;text='{"abi":1,"error":0}'}}
        get {$script:batchReadbacks++;return @{code=0;text=(@{abi=1;error=0;result=@{status='ok';type=4;bytes=4;value=$script:lastBatchValue}}|ConvertTo-Json -Depth 4 -Compress)}}
    }
}
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults @{CpuTune=1;CuMode=40} -Previous @{CpuTune=0;CuMode=24} -Runner $interrupted} 'interrupted default policy cannot report completion'
Check ($script:batchWrites -eq 2 -and $script:batchReadbacks -eq 1) 'only an acknowledged setting receives readback before batch interruption'
$snapshot=[pscustomobject]@{items=@((Item CpuTune setting absent $null),(Item LastStage record ok 50))}
$values=ConvertFrom-DriverStateSnapshot -Snapshot $snapshot
Check (-not $values.ContainsKey('CpuTune') -and $values.LastStage -eq 50) 'only explicit absence omits a value'
$snapshot.items[0].effective.status='access'
Refuses {ConvertFrom-DriverStateSnapshot -Snapshot $snapshot} 'reader does not hide denied access'

Check ((Test-InstalledDriverVersion '0.7.216.100' '0.7.216.100' '' '').ok) 'legacy exact quad'
Check ((Test-InstalledDriverVersion '40.7.216.100' '0.7.216.100' '0.7.216.100' '40.7.216.100').ok) 'legacy rewrite privately proven'
Check (-not (Test-InstalledDriverVersion '40.7.216.100' '0.7.216.100' '' '').ok) 'plus40 without records rejected'
Check (-not (Test-InstalledDriverVersion '40.7.216.100' '0.7.216.100' '0.7.216.101' '40.7.216.100').ok) 'wrong original rejected'
Check (-not (Test-InstalledDriverVersion '40.7.216.100' '0.7.216.100' '0.7.216.100' '40.7.216.101').ok) 'wrong reported rejected'
Check (-not (Test-InstalledDriverVersion '40.7.216.100' '0.7.216.100' '0.7.216.100' '40.7.216.100' $true).ok) 'new identity never accepts runtime alias'
Check ((Test-InstalledDriverVersion '40.7.216.102' '40.7.216.102' '' '' $true).ok) 'installation owned mapped version'
Check (-not (Test-InstalledDriverVersion 'garbage' '0.7.216.100' '0.7.216.100' 'garbage').ok) 'noncanonical alias rejected'
Check (-not (Test-InstalledDriverVersion '' '0.7.216.100' '' '').ok) 'missing property rejected'
Check ((Test-InstalledDriverVersion '40.7.216.100' '0.7.216.100' '0.7.216.100' '40.7.216.100').ok) 'cleared toggle with stale proven alias remains readable'
$script:stored=$null;$script:badReadback=$false
$mutable={param($exe,$arguments)
    switch($arguments[1]) {
        set {$script:stored=[uint32]$arguments[3];return @{code=0;text='{"abi":1,"error":0}'}}
        clear {$script:stored=$null;return @{code=0;text='{"abi":1,"error":0}'}}
        get {return @{code=0;text=([ordered]@{abi=1;error=0;result=@{status='ok';type=4;bytes=4;
            value=$(if($script:badReadback){999}else{$script:stored})}} | ConvertTo-Json -Depth 5 -Compress)}}
        snapshot {return @{code=0;text=([ordered]@{abi=1;error=0;items=@((Item CpuTune setting $(if($null -eq $script:stored){'absent'}else{'ok'}) $script:stored))}|ConvertTo-Json -Depth 6 -Compress)}}
    }
}
Write-DriverStateValue -Name CpuTune -Value 0 -Cli fixture.exe -Runner $mutable
Check ($script:stored -eq 0) 'writer preserves explicit zero with readback'
Clear-DriverStateValue -Name CpuTune -Cli fixture.exe -Runner $mutable
Check ($null -eq $script:stored) 'clear confirms absence of override'
$script:badReadback=$true
Refuses {Write-DriverStateValue -Name CpuTune -Value 1 -Cli fixture.exe -Runner $mutable} 'writer refuses mismatched readback'
$members=@('bc250kmd.inf','bc250kmd.sys','bc250kmd.cat')
Assert-DriverStatePackageMembers -Names $members -InfText '[Version]'
Check $true 'old package member shape retained'
Refuses {Assert-DriverStatePackageMembers -Names $members -InfText 'HKR,, AmdgpuStateCatalog, 0x00000000, "%13%\amdgpu-state.bin"'} 'new INF cannot stage without state catalog'
Assert-DriverStatePackageMembers -Names ($members+@('amdgpu-state.bin')) -InfText 'HKR,, AmdgpuStateCatalog, 0x00000000, "%13%\amdgpu-state.bin"'
Check $true 'new package stages its catalog'
"state lifecycle: $script:checks checks PASS"
# Separate script scope so its memory-only native adapter cannot leak into these tests.
& "$PSScriptRoot\test-continuation-settings.ps1"
