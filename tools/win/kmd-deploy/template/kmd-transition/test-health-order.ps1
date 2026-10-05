$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$saved=@{umd_registration=@('cpu');icd_registration=@('icd');umd_sha256='CPU'}
$o=@{umd_registration=@('cpu');icd_registration=@('icd');parameters=@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=1};dwm=@(@{pid=42;start='start';modules=@(@{name='bc250d3d.dll';sha256='CPU'})})}
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
Assert-KmdCpuBaseline $saved $o -AllowUnconfirmed
Must-Reject {Assert-KmdCpuBaseline $saved $o}
$r=Wait-KmdCpuBaseline $saved -Read {$o} -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+[Diagnostics.Stopwatch]::Frequency) -IntervalMs 1 -AllowUnconfirmed
if($r.attempts -ne 2){throw 'Unconfirmed but ready DWM was not admitted'}
$before=Get-KmdReadyHealth 'health abi=1 version=0x000700AE flags=7 generation=123 epoch=5 completed=1 age_ms=15000 ready_ms=60000' '0x000700AE'
$after=Get-KmdReadyHealth 'health abi=1 version=0x000700AE flags=15 generation=123 epoch=5 completed=2 age_ms=0 ready_ms=60001' '0x000700AE'
Assert-KmdConfirmedHealth $before $after
Must-Reject {Assert-KmdConfirmedHealth $before $before}
$wrong=@{flags=15;generation=124;epoch=5}
Must-Reject {Assert-KmdConfirmedHealth $before $wrong}
$wrong=@{flags=15;generation=123;epoch=6}
Must-Reject {Assert-KmdConfirmedHealth $before $wrong}
Must-Reject {Get-KmdReadyHealth 'health abi=1 version=0x000700AD flags=7 generation=123 epoch=5 completed=1 age_ms=15000 ready_ms=60000' '0x000700AE'}
$o.parameters.UnconfirmedStarts=0
Assert-KmdCpuBaseline $saved $o
$o.parameters.UnconfirmedStarts=3
Must-Reject {Assert-KmdCpuBaseline $saved $o -AllowUnconfirmed}
Assert-KmdConfirmEligible $before
foreach($pair in @(@('ready_ms',59999),@('age_ms',15001),@('completed',0),@('flags',3))) {
 $bad=$before.Clone();$bad[$pair[0]]=$pair[1]
 Must-Reject {Assert-KmdConfirmEligible $bad}
}
$missing=$before.Clone();$missing.Remove('ready_ms')
Must-Reject {Assert-KmdConfirmEligible $missing}
Must-Reject {Get-KmdReadyHealth 'health abi=1 version=0x000700AE flags=7 generation=123 epoch=5 completed=1' '0x000700AE'}
Must-Reject {Get-KmdReadyHealth ("health abi=1 version=0x000700AE flags=7 generation=1 epoch=5 completed=1 age_ms=0 ready_ms=60000`nhealth abi=1 version=0x000700AE flags=7 generation=1 epoch=5 completed=1 age_ms=0 ready_ms=60000") '0x000700AE'}
$wide=Get-KmdReadyHealth 'health abi=1 version=0x000700AE flags=7 generation=4294967296 epoch=4294967297 completed=1 age_ms=0 ready_ms=60000' '0x000700AE'
if($wide.generation -ne 4294967296 -or $wide.epoch -ne 4294967297){throw '64-bit identity truncated'}
'PASS: readiness, strict confirmation age/freshness boundaries, identity and guard checks'
