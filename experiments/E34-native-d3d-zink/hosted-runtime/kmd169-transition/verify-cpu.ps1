# Pure acceptance check; the caller gathers fresh OS observations.
function Assert-KmdCpuBaseline {
 param($Saved,$Observed)
 foreach($name in @('umd_registration','icd_registration')) {
  $expected=@($Saved.$name);$actual=@($Observed.$name)
  if(!$expected.Count -or $actual.Count -ne $expected.Count){throw "Registration count mismatch: $name"}
  for($i=0;$i -lt $expected.Count;$i++) {
   if([string]::IsNullOrWhiteSpace([string]$actual[$i]) -or $actual[$i] -ine $expected[$i]){throw "Registration mismatch: $name"}
  }
 }
 foreach($name in @('EnableGpuPresentBlit','EnableCddDwmInterop','UnconfirmedStarts')) {
  if($null -eq $Observed.parameters.$name -or $Observed.parameters.$name -ne 0){throw "Baseline gate/guard mismatch: $name"}
 }
 $desktop=@($Observed.dwm)
 if(!$desktop.Count){throw 'No DWM observed'}
 foreach($process in $desktop) {
  if(!$process.pid -or !$process.start){throw 'Missing DWM identity'}
  $modules=@($process.modules)
  $cpu=@($modules | Where-Object {$_.name -ieq 'bc250d3d.dll' -and $_.sha256 -eq $Saved.umd_sha256})
  if($cpu.Count -ne 1){throw 'DWM CPU UMD not witnessed'}
  foreach($module in $modules) {
   if($module.name -ine 'bc250d3d.dll' -or $module.sha256 -ne $Saved.umd_sha256){throw 'Unexpected DWM graphics module'}
  }
 }
}
