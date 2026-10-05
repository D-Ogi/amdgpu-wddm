$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$saved=@{umd_registration=@('cpu.dll');icd_registration=@('base.json');umd_sha256='CPU'}
function Sample([int]$Id){
 return @{umd_registration=@('cpu.dll');icd_registration=@('base.json');
  parameters=@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=0};
  dwm=@(@{pid=$Id;start='start';modules=@(@{name='bc250d3d.dll';sha256='CPU'})})}
}
$state=@{count=0;records=(New-Object Collections.ArrayList)}
$deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[Diagnostics.Stopwatch]::Frequency*2
$r=Wait-KmdCpuBaseline $saved -Deadline $deadline -IntervalMs 1 -Read {
 $state.count++
 switch($state.count){
  1 {throw 'DWM exited during module enumeration'}
  2 {$s=Sample 1;$s.dwm=@();return $s}
  3 {return (Sample 1)}
  default {return (Sample 2)}
 }
} -Record {param($r);[void]$state.records.Add($r)}
if($r.attempts -ne 5 -or $r.observed.dwm[0].pid -ne 2 -or $state.records.Count -ne 5){throw 'Restart was not resampled'}
if(!$r.first_ready_qpc -or $r.stable_qpc -lt $r.first_ready_qpc){throw 'Missing readiness timing'}
$deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long]([Diagnostics.Stopwatch]::Frequency*0.05)
$failed=$false
try{Wait-KmdCpuBaseline $saved -Deadline $deadline -IntervalMs 1 -Read {$s=Sample 1;$s.dwm[0].modules[0].sha256='WRONG';return $s}|Out-Null}catch{$failed=$true}
if(!$failed){throw 'Incorrect UMD accepted at deadline'}
$called=@{read=$false}
try{Wait-KmdCpuBaseline $saved -Deadline 1 -Read {$called.read=$true;return (Sample 1)}|Out-Null}catch{}
if($called.read){throw 'Read started after deadline'}
'PASS: delayed attachment, disappearing/restarting DWM, persistent mismatch, expired deadline'
