$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$saved=@{umd_registration=@('cpu');icd_registration=@('icd');umd_sha256='CPU'}
$o=@{umd_registration=@('cpu');icd_registration=@('icd');parameters=@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=1};dwm=@(@{pid=42;start='start';modules=@(@{name='bc250d3d.dll';sha256='CPU'})})}
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
Assert-KmdCpuBaseline $saved $o -AllowUnconfirmed
Must-Reject {Assert-KmdCpuBaseline $saved $o}
$r=Wait-KmdCpuBaseline $saved -Read {$o} -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+[Diagnostics.Stopwatch]::Frequency) -IntervalMs 1 -AllowUnconfirmed
if($r.attempts -ne 2){throw 'Unconfirmed but ready DWM was not admitted'}
$before=Get-KmdReadyHealth 'health abi=1 version=0x000700A9 flags=7 generation=123 epoch=5 completed=1' '0x000700A9'
$after=Get-KmdReadyHealth 'health abi=1 version=0x000700A9 flags=15 generation=123 epoch=5 completed=2' '0x000700A9'
Assert-KmdConfirmedHealth $before $after
Must-Reject {Assert-KmdConfirmedHealth $before $before}
$wrong=@{flags=15;generation=124;epoch=5}
Must-Reject {Assert-KmdConfirmedHealth $before $wrong}
$wrong=@{flags=15;generation=123;epoch=6}
Must-Reject {Assert-KmdConfirmedHealth $before $wrong}
Must-Reject {Get-KmdReadyHealth 'health abi=1 version=0x000700A6 flags=7 generation=123 epoch=5 completed=1' '0x000700A9'}
$o.parameters.UnconfirmedStarts=0
Assert-KmdCpuBaseline $saved $o
$o.parameters.UnconfirmedStarts=3
Must-Reject {Assert-KmdCpuBaseline $saved $o -AllowUnconfirmed}
'PASS: M711 guard1 readiness, checked generation/epoch confirmation, strict final guard0'
