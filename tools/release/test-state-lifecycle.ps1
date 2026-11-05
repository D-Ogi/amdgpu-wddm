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
Refuses {Complete-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.103' -Device $device -InfSha256 $sha -Save $save -Runner $run} 'different package cannot acknowledge'
Refuses {Complete-DriverStateTransaction -State $s -Worker fixture.exe -Product '0.7.216.102' -Device $device -InfSha256 ('B'*64) -Save $save -Runner $run} 'different INF cannot acknowledge'
Check ($s.driver_state.phase -eq 'prepared' -and $s.driver_state.transaction -ceq $tx) 'refused acknowledgments leave the transaction'
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

# BD-298: an install that stopped after its prepare (state.json driver_state phase prepared) is abandoned by the
# uninstall and by the install of another package; the exact package still resumes it; 'abandoned' is terminal.
$script:calls=New-Object Collections.Generic.List[string];$script:saves=New-Object Collections.Generic.List[string]
$script:abandonCode=0
$rec={param($exe,$arguments)
    $script:calls.Add("$exe " + ($arguments -join ' '))
    if($arguments[0] -eq 'abandon' -and $script:abandonCode){return @{code=$script:abandonCode;text="{`"abi`":1,`"error`":$($script:abandonCode)}"}}
    return @{code=0;text='{"abi":1,"error":0}'}
}
$rsave={param($st) $script:saves.Add([string]$st.driver_state.phase)}
$stopped=('e5'*16)
function Stopped([string]$Product='0.7.216.102') {
    [pscustomobject]@{phase='install-incomplete';driver_state=[pscustomobject]@{schema=2;transaction=$stopped;phase='prepared';
        product=$Product;device=$device;inf_sha256=$sha}}
}
function Reset298 {$script:calls.Clear();$script:saves.Clear();$script:abandonCode=0}
# Every phase and intent has one step in the table; an unknown phase and a damaged record are refused.
foreach($phase in @('prepared','acknowledged','abandoned')) {
    foreach($intent in @('install','complete','remove')) {
        $p=Stopped;$p.driver_state.phase=$phase
        [void](Get-DriverStateStep -Record $p.driver_state -Intent $intent -Product '0.7.216.102' -Device $device -InfSha256 $sha)
    }
}
Check ((Get-DriverStateStep -Record $null -Intent install) -eq 'begin') 'no record begins a transaction'
Check ((Get-DriverStateStep -Record (Stopped).driver_state -Intent install -Product '0.7.216.102' -Device $device -InfSha256 $sha) -eq 'resume') 'exact package resumes'
Check ((Get-DriverStateStep -Record (Stopped).driver_state -Intent install -Product '0.7.216.110' -Device $device -InfSha256 $sha) -eq 'abandon') 'another product abandons'
Check ((Get-DriverStateStep -Record (Stopped).driver_state -Intent install -Product '0.7.216.102' -Device $device -InfSha256 ('B'*64)) -eq 'abandon') 'another INF abandons'
Check ((Get-DriverStateStep -Record (Stopped).driver_state -Intent install -Product '0.7.216.102' -Device 'PCI\VEN_1002&DEV_13FE\OTHER' -InfSha256 $sha) -eq 'abandon') 'another device abandons'
Check ((Get-DriverStateStep -Record (Stopped).driver_state -Intent remove) -eq 'abandon') 'uninstall abandons'
$p=Stopped;$p.driver_state.phase='half-done'
Refuses {Get-DriverStateStep -Record $p.driver_state -Intent install} 'unknown phase refused'
$p=Stopped;$p.driver_state.schema=1
Refuses {Get-DriverStateStep -Record $p.driver_state -Intent remove} 'record of another schema refused'
$p=Stopped;$p.driver_state.transaction='../x'
Refuses {Get-DriverStateStep -Record $p.driver_state -Intent remove} 'malformed transaction refused'

# The exact package resumes the same transaction: no abandon step.
Reset298;$p=Stopped
[void](Start-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $rsave -Runner $rec)
Check ($p.driver_state.transaction -ceq $stopped -and $p.driver_state.phase -eq 'prepared') 'exact package keeps its transaction'
Check ($script:calls.Count -eq 1 -and $script:calls[0] -ceq "new.exe prepare $stopped 0.7.216.102 $device") 'exact package only repeats its prepare'

# The install of another package: the worker abandons the old transaction with its own identity, the record is
# abandoned durably and kept in the history, and only then does the new transaction begin.
Reset298;$p=Stopped
$log298=Join-Path ([IO.Path]::GetTempPath()) ("bd298-" + [guid]::NewGuid().ToString('N') + '.log');$script:LogPath=$log298
try {
    [void](Start-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.110' -Device $device -InfSha256 ('C'*64) -Save $rsave -Runner $rec -StateDirectory ([IO.Path]::GetTempPath()))
    $text=[IO.File]::ReadAllText($log298)
} finally { $script:LogPath=$null; Remove-Item -LiteralPath $log298 -ErrorAction SilentlyContinue }
Check ($script:calls.Count -eq 2 -and $script:calls[0] -ceq "new.exe abandon $stopped 0.7.216.102 $device" -and
    $script:calls[1] -like "new.exe prepare * 0.7.216.110 $device") 'abandon of the old identity precedes the new prepare'
Check ($script:saves.Count -eq 2 -and $script:saves[0] -eq 'abandoned' -and $script:saves[1] -eq 'prepared') 'abandoned is durable before the new transaction'
Check ($p.driver_state.transaction -cne $stopped -and $p.driver_state.product -eq '0.7.216.110' -and $p.driver_state.phase -eq 'prepared') 'new transaction of the new package'
$h=@($p.driver_state_history)
Check ($h.Count -eq 1 -and $h[0].transaction -ceq $stopped -and $h[0].phase -eq 'abandoned' -and $h[0].product -eq '0.7.216.102' -and
    $h[0].inf_sha256 -eq $sha -and $h[0].abandoned_for -eq 'install of 0.7.216.110' -and $h[0].abandoned_utc) 'history keeps the abandoned identity'
Check ($text -match 'WARNING: The install of driver version 0\.7\.216\.102 did not finish' -and $text -match 'kept as evidence' -and
    $text -match "retired-$stopped-pending\.bin") 'the abandon is a warning with the evidence name in the log'

# A second abandon of the same transaction (its handoff published again): the worker removes a pending.bin identical to
# the retired copy, and the log says so with the hash of the retired copy.
Reset298;$p=Stopped
$dir298=Join-Path ([IO.Path]::GetTempPath()) ("bd298-" + [guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Force $dir298 | Out-Null
$script:LogPath=Join-Path $dir298 'install.log'
try {
    [IO.File]::WriteAllBytes((Join-Path $dir298 'pending.bin'),[byte[]](1,2,3));[IO.File]::WriteAllBytes((Join-Path $dir298 "retired-$stopped-pending.bin"),[byte[]](1,2,3))
    $removePending={param($exe,$arguments) $script:calls.Add("$exe " + ($arguments -join ' '));Remove-Item -LiteralPath (Join-Path $dir298 'pending.bin') -ErrorAction SilentlyContinue;@{code=0;text='{"abi":1,"error":0}'}}
    [void](Start-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.110' -Device $device -InfSha256 $sha -Save $rsave -Runner $removePending -StateDirectory $dir298)
    $text=[IO.File]::ReadAllText($script:LogPath)
} finally { $script:LogPath=$null; Remove-Item -LiteralPath $dir298 -Recurse -Force -ErrorAction SilentlyContinue }
Check ($text -match 'identical to the retired copy' -and $text -match "retired-$stopped-pending\.bin SHA256 039058C6F2C0CB492C533B0A4D14EF77CC0F78ABCCCED5287D84A1A2011CFB81") 'second abandon: the removal of the identical pending file is in the log'
# The driver began the first transaction of the store and stopped before Active (lab 26200): the log says so, and
# the store stays.
Reset298;$p=Stopped
$key298="HKCU:\Software\bd298-test-$([guid]::NewGuid().ToString('N'))";[void](New-Item -Path "$key298\Generations\$stopped\Migration" -Force)
$script:LogPath=Join-Path ([IO.Path]::GetTempPath()) ("bd298-" + [guid]::NewGuid().ToString('N') + '.log')
try {
    Stop-DriverStateTransaction -State $p -Worker new.exe -Reason install -Product '0.7.216.110' -Save $rsave -Runner $rec -StateDirectory ([IO.Path]::GetTempPath()) -StoreKey $key298
    $text=[IO.File]::ReadAllText($script:LogPath);$kept=Test-Path -LiteralPath $key298
} finally { Remove-Item -LiteralPath $script:LogPath -ErrorAction SilentlyContinue;$script:LogPath=$null;Remove-Item -LiteralPath $key298 -Recurse -Force -ErrorAction SilentlyContinue }
Check ($p.driver_state.phase -eq 'abandoned' -and $kept -and $text -match 'stopped before it completed' -and $text -match 'reads the legacy settings') 'unfinished store: the log says so and the store stays'
$p=Stopped;$script:LogPath=Join-Path ([IO.Path]::GetTempPath()) ("bd298-" + [guid]::NewGuid().ToString('N') + '.log')
try { Stop-DriverStateTransaction -State $p -Worker new.exe -Reason install -Product '0.7.216.110' -Save $rsave -Runner $rec -StateDirectory ([IO.Path]::GetTempPath()) -StoreKey 'HKCU:\Software\bd298-test-absent'
    $text=[IO.File]::ReadAllText($script:LogPath) } finally { Remove-Item -LiteralPath $script:LogPath -ErrorAction SilentlyContinue;$script:LogPath=$null }
Check ($text -notmatch 'stopped before it completed') 'no store: no line about an unfinished store'# A failed abandon leaves the record prepared, starts no transaction and names the cause.
foreach($case in @(@{code=13;text='damaged'},@{code=1306;text='another install'},@{code=183;text='other content'},@{code=5;text='worker failed'})) {
    Reset298;$p=Stopped;$script:abandonCode=$case.code;$msg=''
    try {[void](Start-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.110' -Device $device -InfSha256 $sha -Save $rsave -Runner $rec)} catch {$msg=$_.Exception.Message}
    Check ($msg -match 'cannot be abandoned' -and $msg -match $case.text -and $msg -match '0\.7\.216\.102') "failed abandon (exit $($case.code)) is a clear refusal"
    Check ($p.driver_state.phase -eq 'prepared' -and $p.driver_state.transaction -ceq $stopped -and $script:saves.Count -eq 0 -and
        @($script:calls | Where-Object {$_ -like '* prepare *'}).Count -eq 0) "failed abandon (exit $($case.code)) changes nothing"
}
# The retry after a failed abandon repeats it and then succeeds.
$script:abandonCode=0
[void](Start-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.110' -Device $device -InfSha256 $sha -Save $rsave -Runner $rec)
Check ($p.driver_state.phase -eq 'prepared' -and $p.driver_state.transaction -cne $stopped -and @($p.driver_state_history).Count -eq 1) 'retry after a failed abandon finishes'

# 'abandoned' is terminal: no acknowledgment, and a new install begins a new transaction, also for the old package.
Reset298;$p=Stopped;$p.driver_state.phase='abandoned'
Refuses {Complete-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $rsave -Runner $rec} 'abandoned transaction cannot be acknowledged'
[void](Start-DriverStateTransaction -State $p -Worker new.exe -Product '0.7.216.102' -Device $device -InfSha256 $sha -Save $rsave -Runner $rec)
Check ($p.driver_state.transaction -cne $stopped -and $script:calls.Count -eq 1 -and $script:calls[0] -like 'new.exe prepare *') 'abandoned transaction is never resumed'

# The uninstall over a stopped install: abandon with the given worker, then the normal archive with the installed one.
Reset298;$p=Stopped
Save-DriverStateBeforeRemoval -State $p -Worker installed.exe -AbandonWorker new.exe -Product '0.7.216.109' -Device $device -Save $rsave -Runner $rec
Check ($script:calls.Count -eq 2 -and $script:calls[0] -ceq "new.exe abandon $stopped 0.7.216.102 $device" -and
    $script:calls[1] -like "installed.exe archive * 0.7.216.109 $device") 'uninstall abandons and then archives'
Check ($p.driver_state.phase -eq 'abandoned' -and $p.driver_state_removal.phase -eq 'archived' -and @($p.driver_state_history)[0].abandoned_for -eq 'uninstall') 'uninstall continues its removal'
# A resumed uninstall does not abandon again.
$script:calls.Clear()
Save-DriverStateBeforeRemoval -State $p -Worker installed.exe -Product '0.7.216.109' -Device $device -Save $rsave -Runner $rec
Check ($script:calls.Count -eq 1 -and $script:calls[0] -like 'installed.exe archive *') 'resumed uninstall only archives'
# A failed abandon stops the uninstall before the archive.
Reset298;$p=Stopped;$script:abandonCode=13
Refuses {Save-DriverStateBeforeRemoval -State $p -Worker installed.exe -Product '0.7.216.109' -Device $device -Save $rsave -Runner $rec} 'failed abandon stops the uninstall'
Check ($p.driver_state.phase -eq 'prepared' -and -not $p.driver_state_removal -and @($script:calls | Where-Object {$_ -like '* archive *'}).Count -eq 0) 'no removal step after a failed abandon'
# install.ps1 abandons before the pre-stage settings comparison, which refuses a prepared record without its own comparison.
$installText=[IO.File]::ReadAllText("$PSScriptRoot\installer\install.ps1")
$iResolve=$installText.IndexOf('Resolve-DriverStateTransaction -State $state');$iCompare=$installText.IndexOf('Save-DriverSettingsComparison -State $state')
Check ($iResolve -gt 0 -and $iResolve -lt $iCompare) 'install abandons a stopped install before its settings comparison'
$uninstallText=[IO.File]::ReadAllText("$PSScriptRoot\installer\uninstall.ps1")
Check ($uninstallText.Contains('-AbandonWorker $abandonWorker')) 'uninstall gives the worker of its own package for the abandon step'
# BD-262: a removal record that the prepare moved aside (an unreadable catalog, fresh handoff) is a warning in the log.
# A record that the prepare read by setting name (handoff origin 4) and a clean computer give no warning.
$dir=Join-Path $(if($env:BC250_TEST_OUT){$env:BC250_TEST_OUT}else{[IO.Path]::GetTempPath()}) ("state-retired-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $dir | Out-Null
$script:LogPath=Join-Path $dir 'install.log'
try {
    function Pending([uint32]$Origin){$b=New-Object byte[] 360;[BitConverter]::GetBytes([uint32]0x32545341).CopyTo($b,0)
        [BitConverter]::GetBytes($Origin).CopyTo($b,16);[IO.File]::WriteAllBytes((Join-Path $dir 'pending.bin'),$b)}
    function Prepared {
        $p=[pscustomobject]@{driver_state=[pscustomobject]@{schema=2;transaction=('ab'*16);phase='prepared';
            product='0.7.216.106';device=$device;inf_sha256=$sha}}
        [void](Start-DriverStateTransaction -State $p -Worker fixture.exe -Product '0.7.216.106' -Device $device -InfSha256 $sha `
            -Save $save -Runner $run -StateDirectory $dir)
        $text=if(Test-Path -LiteralPath $script:LogPath){[IO.File]::ReadAllText($script:LogPath)}else{''}
        Remove-Item -LiteralPath $script:LogPath -ErrorAction SilentlyContinue
        return $text
    }
    Pending 1
    Check (-not ((Prepared) -match 'WARNING')) 'fresh install without a moved record has no warning'
    [IO.File]::WriteAllBytes((Join-Path $dir "retired-$('ab'*16)-removed.bin"),[byte[]](1,2,3))
    $log=Prepared
    Check ($log -match 'WARNING: The saved settings of the driver version that was removed before this install cannot be carried over' -and
        $log -match 'default settings' -and $log -match "retired-$('ab'*16)-removed\.bin") 'moved record is a warning with the file name in the log'
    Pending 4
    Check (-not ((Prepared) -match 'WARNING')) 'record read by setting name has no warning'
    Pending 1
    Check (@(Get-DriverStateRetiredRecords -Transaction ('cd'*16) -Directory $dir).Count -eq 0) 'record of another transaction is not this install'
} finally { $script:LogPath=$null; Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue }

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
# The real shared-state API denies user mutation of DpmClosedReason. Repair
# must request reopening through settings, including after a partial prior run.
$catalog=Get-Content -LiteralPath "$PSScriptRoot\..\..\driver\state\catalog.json" -Raw | ConvertFrom-Json
$reasons=@{DpmClosedReason=4;InteropClosedReason=4;CuModeClosedReason=3}
$reasonAccess=@{DpmClosedReason='driver';InteropClosedReason='admin_clear';CuModeClosedReason='admin_clear'}
foreach($name in $reasons.Keys) {
    $entry=@($catalog.values | Where-Object {$_.path -ceq '' -and $_.name -ceq $name})
    Check ($entry.Count -eq 1 -and $entry[0].kind -eq 'record' -and $entry[0].access -eq $reasonAccess[$name]) "$name retains its catalog access contract"
}
$openDefaults=@{DpmMode=1;EnableGpuPresentBlit=1;EnableCddDwmInterop=1;CuMode=40}
$script:closureLive=@{DpmMode=0;EnableGpuPresentBlit=0;EnableCddDwmInterop=0;CuMode=24}
foreach($name in $reasons.Keys){$script:closureLive[$name]=$reasons[$name]}
$script:closureCalls=New-Object Collections.Generic.List[string]
$script:closureFailSet=0;$script:closureSets=0
$closureRun={param($exe,$arguments)
    switch($arguments[1]) {
        snapshot {
            $rows=@(foreach($name in $script:closureLive.Keys){
                Item $name $(if($reasons.ContainsKey($name)){'record'}else{'setting'}) ok $script:closureLive[$name]
            })
            return @{code=0;text=(@{abi=1;error=0;items=$rows}|ConvertTo-Json -Depth 7 -Compress)}
        }
        set {
            $script:closureCalls.Add(($arguments -join ' '));$script:closureSets++
            if($reasons.ContainsKey($arguments[2]) -or $script:closureSets -eq $script:closureFailSet){return @{code=5;text='access denied'}}
            $script:closureLive[$arguments[2]]=[uint32]$arguments[3]
            return @{code=0;text='{"abi":1,"error":0}'}
        }
        clear {$script:closureCalls.Add(($arguments -join ' '));return @{code=5;text='driver-owned record'}}
        get {return @{code=0;text=(@{abi=1;error=0;result=@{status='ok';type=4;bytes=4;value=$script:closureLive[$arguments[2]]}}|ConvertTo-Json -Depth 5 -Compress)}}
        default {throw 'unexpected closure fixture command'}
    }
}
Invoke-DriverStateDefaults -Cli fixture.exe -Defaults $openDefaults -Previous $openDefaults -Runner $closureRun
Check ($script:closureCalls.Count -eq 0) 'ordinary install preserves driver closures'
$script:closureFailSet=2
Refuses {Invoke-DriverStateDefaults -Cli fixture.exe -Defaults $openDefaults -Previous $openDefaults -Reopen -FreshReopen -Runner $closureRun} 'failed reopen setting write remains a failure'
foreach($name in $reasons.Keys){Check ($script:closureLive[$name] -eq $reasons[$name]) "partial repair retains $name"}
$script:closureFailSet=0
Invoke-DriverStateDefaults -Cli fixture.exe -Defaults $openDefaults -Previous $openDefaults -Reopen -FreshReopen -Runner $closureRun
foreach($name in $openDefaults.Keys){Check ($script:closureLive[$name] -eq $openDefaults[$name]) "repair reopens $name through its setting"}
foreach($name in $reasons.Keys){Check ($script:closureLive[$name] -eq $reasons[$name]) "successful repair retains $name until KMD initialization"}
Check (@($script:closureCalls | Where-Object {$_ -like 'state clear *'}).Count -eq 0) 'repair never asks the CLI to clear a driver-owned record'
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
