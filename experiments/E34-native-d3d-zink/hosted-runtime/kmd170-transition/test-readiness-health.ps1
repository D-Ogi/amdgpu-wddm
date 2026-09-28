$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$saved=@{umd_registration=@('cpu');icd_registration=@('icd');umd_sha256='CPU'}
$cpuFixture=@{umd_registration=@('cpu');icd_registration=@('icd');parameters=@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=1};dwm=@(@{pid=42;start='start';modules=@(@{name='bc250d3d.dll';sha256='CPU'})})}
function Run-Sequence([object[]]$Sequence,[int]$Expected){
 $script:attempt=0
 $r=Wait-KmdCpuBaseline -Saved $saved -Read {$cpuFixture} -AllowUnconfirmed -IntervalMs 1 -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+2*[Diagnostics.Stopwatch]::Frequency) -ReadHealth {
  $entry=$Sequence[$script:attempt];$script:attempt++
  if($entry -eq 'error'){throw 'Escape temporarily unavailable'}
  @{generation=123;epoch=[uint64]$entry;flags=7;completed=1;age_ms=0;ready_ms=100}
 }
 if($r.attempts -ne $Expected -or $r.health.epoch -ne 5){throw 'Premature or wrong settled anchor'}
}
Run-Sequence @('error',3,4,5,5) 5
Run-Sequence @(5,'error',5,5) 4
Run-Sequence @(5,5) 2
$script:attempt=0
$r=Wait-KmdCpuBaseline -Saved $saved -Read {
 $script:attempt++
 $cpuFixture.dwm[0].pid=if($script:attempt -eq 1){41}else{42}
 $cpuFixture
} -AllowUnconfirmed -IntervalMs 1 -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+2*[Diagnostics.Stopwatch]::Frequency) -ReadHealth {
 @{generation=123;epoch=5;flags=15;completed=1;age_ms=0;ready_ms=60000}
}
if($r.attempts -ne 3){throw 'DWM change did not reset stable pair'}
'PASS: transient health failure, expected startup epochs, gap reset and joint DWM/health stability'
