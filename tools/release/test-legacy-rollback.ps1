# Actual bridge functions with native/storage adapters replaced by fixtures.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\installer\common.ps1"
. "$PSScriptRoot\installer\legacy-rollback.ps1"
$script:checks=0
function Check([bool]$ok,[string]$name){$script:checks++;if(-not $ok){throw "FAIL: $name"}}
function Refuses([scriptblock]$body,[string]$name){$refused=$false;try{& $body}catch{$refused=$true};Check $refused $name}
$script:events=New-Object Collections.Generic.List[string]
$script:bad=''
$runner={param($exe,$a) $script:events.Add($a[0]);if($script:bad -eq $a[0]){return @{code=5;text='injected refusal'}};return @{code=0;text='{"abi":1,"error":0}'}}
$save={param($s) $script:events.Add('save:'+$s.legacy_rollback.phase)}
$target=[pscustomobject]@{product='0.7.216.102';inf_sha256=('1'*64);sys_sha256=('2'*64);manifest_sha256=('3'*64);
    inf_identity=@{date='10/13/2026';quad='40.7.216.102'}}
$state=[pscustomobject]@{driver_state=@{phase='acknowledged'}}
$device='PCI\VEN_1002&DEV_13FE\FIXTURE'
$r=Start-LegacyRollback -State $state -Target $target -Device $device -SourceInf oem17.inf -Worker fixture.exe -Save $save -Runner $runner
Check (($script:events -join ',') -eq 'save:capture,rollback-capture,save:captured') 'identity persisted before native freeze and final capture'
$tx=$r.transaction
[void](Start-LegacyRollback -State $state -Target $target -Device $device -Worker fixture.exe -Save $save -Runner $runner)
Check ($state.legacy_rollback.transaction -ceq $tx) 'resume keeps original identity'
Refuses {Complete-LegacyRollback $state fixture.exe enable $save $runner} 'enable cannot precede restore'
$target.manifest_sha256='4'*64
Refuses {Start-LegacyRollback -State $state -Target $target -Device $device -Worker fixture.exe -Save $save -Runner $runner} 'different manifest refuses before native changes'
$target.manifest_sha256='3'*64
$script:bad='rollback-restore'
Refuses {Complete-LegacyRollback $state fixture.exe restore $save $runner} 'restore error is not acknowledgment'
Check ($state.legacy_rollback.phase -eq 'captured') 'failed restore cannot enable'
$script:bad='';Complete-LegacyRollback $state fixture.exe restore $save $runner
Check ($state.legacy_rollback.phase -eq 'restored') 'restoration acknowledged'
$script:bad='rollback-enable'
Refuses {Complete-LegacyRollback $state fixture.exe enable $save $runner} 'enable failure keeps recovery receipt'
Check ($state.legacy_rollback.phase -eq 'restored') 'failure has no completed claim'
$script:bad='';Complete-LegacyRollback $state fixture.exe enable $save $runner
Check ($state.legacy_rollback.phase -eq 'enabled') 'only final native acknowledgment permits enabled phase'
$state.legacy_rollback.phase='captured'
$script:inventory=@([pscustomobject]@{published='oem17.inf';date='10/14/2026';version='40.7.216.103'},
    [pscustomobject]@{published='oem19.inf';date='10/14/2026';version='40.7.216.104'})
$readInventory={return $script:inventory};$hash={param($n) return ('8'*64)}
$script:events.Clear()
Refuses {Remove-LegacyRollbackDriverSets $state fixture.exe $readInventory $hash $runner} 'other higher package refuses'
Check (-not $script:events.Contains('rollback-remove')) 'competing package never broadens removal'
$script:inventory=@($script:inventory[0])
$removeRunner={param($exe,$a) $script:events.Add($a[0]);if($a[0] -eq 'rollback-remove'){$script:inventory=@()};return @{code=0;text='{"abi":1,"error":0}'}}
$script:events.Clear()
Remove-LegacyRollbackDriverSets $state fixture.exe $readInventory $hash $removeRunner
Check (($script:events -join ',') -eq 'rollback-check,rollback-remove,rollback-check') 'exact source removal is bracketed by stopped-disabled checks'
Refuses {Invoke-LegacyRollbackWorker fixture.exe restore $r {param($e,$a) @{code=0;text='{"abi":1}'}}} 'missing durable reply refuses'
"legacy rollback: $script:checks checks PASS"
