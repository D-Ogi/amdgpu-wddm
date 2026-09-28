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

# Two matching process identities with passing module checks avoid accepting a
# single sample taken while DWM is being replaced. This is not a lifetime promise.
function Wait-KmdCpuBaseline {
 param($Saved,[scriptblock]$Read,[long]$Deadline,[scriptblock]$Record,
       [ValidateRange(1,1000)][int]$IntervalMs=250)
 $attempts=0;$previous=$null;$firstReady=$null
 while([Diagnostics.Stopwatch]::GetTimestamp() -lt $Deadline){
  $attempts++;$sampleQpc=[Diagnostics.Stopwatch]::GetTimestamp()
  $errorText=$null;$identity=$null;$observed=$null
  try {
   $observed=& $Read
   Assert-KmdCpuBaseline $Saved $observed
   $identity=(@($observed.dwm|ForEach-Object {"$($_.pid):$($_.start)"}|Sort-Object) -join '|')
  } catch {$errorText=[string]$_}
  $within=[Diagnostics.Stopwatch]::GetTimestamp() -lt $Deadline
  if($identity -and $null -eq $firstReady){$firstReady=$sampleQpc}
  $stable=$within -and $identity -and $identity -eq $previous
  if($Record){& $Record @{attempt=$attempts;qpc=$sampleQpc;identity=$identity;error=$errorText;stable=[bool]$stable}|Out-Null}
  if($stable){return @{observed=$observed;attempts=$attempts;first_ready_qpc=$firstReady;stable_qpc=[Diagnostics.Stopwatch]::GetTimestamp()}}
  $previous=$identity
  $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())*1000/[double][Diagnostics.Stopwatch]::Frequency
  if($remaining -gt 0){Start-Sleep -Milliseconds ([int][Math]::Min($IntervalMs,[Math]::Ceiling($remaining)))}
 }
 throw "CPU DWM readiness deadline expired after $attempts samples"
}
