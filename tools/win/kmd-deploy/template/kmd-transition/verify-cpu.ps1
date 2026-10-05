# Project only acceptance inputs; never serialize registry provider metadata.
function Select-KmdCpuParameters {
 param($Source)
 return @{
  EnableGpuPresentBlit=$Source.EnableGpuPresentBlit
  EnableCddDwmInterop=$Source.EnableCddDwmInterop
  UnconfirmedStarts=$Source.UnconfirmedStarts
  InteropLastState=$Source.InteropLastState
 }
}

# The desktop the capture names (preflight's `desktop`, from identity.ps1): switches 0 or 1 and the exact module
# set DWM holds. A capture without it (frozen before the GPU DWM ladder) is the CPU UMD alone with switches 0.
function Get-KmdSavedDesktop {
 param($Saved)
 $desktop=$Saved.desktop
 if($null -eq $desktop){return @{switches=0;modules=$null}}
 $modules=@($desktop.modules|ForEach-Object {[string]$_}|Sort-Object -Unique)
 if($desktop.switches -notin @(0,1) -or !$modules.Count){throw 'Invalid saved desktop'}
 return @{switches=[int]$desktop.switches;modules=$modules}
}

# Pure acceptance check; the caller gathers fresh OS observations.
function Assert-KmdCpuBaseline {
 param($Saved,$Observed,[switch]$AllowUnconfirmed)
 foreach($name in @('umd_registration','icd_registration')) {
  $expected=@($Saved.$name);$actual=@($Observed.$name)
  if(!$expected.Count -or $actual.Count -ne $expected.Count){throw "Registration count mismatch: $name"}
  for($i=0;$i -lt $expected.Count;$i++) {
   if([string]::IsNullOrWhiteSpace([string]$actual[$i]) -or $actual[$i] -ine $expected[$i]){throw "Registration mismatch: $name"}
  }
 }
 $saveddesktop=Get-KmdSavedDesktop $Saved
 foreach($name in @('EnableGpuPresentBlit','EnableCddDwmInterop')) {
  if($null -eq $Observed.parameters.$name -or $Observed.parameters.$name -ne $saveddesktop.switches){throw "Baseline gate/guard mismatch: $name"}
 }
 # Switches 1: the start must have latched both on (InteropLastState 0x303); the router follows that latch.
 if($saveddesktop.switches -eq 1 -and $Observed.parameters.InteropLastState -ne 0x303){throw 'Interop latch mismatch: InteropLastState'}
 $guard=$Observed.parameters.UnconfirmedStarts
 if($null -eq $guard -or $guard -notin @(0,1,2) -or (!$AllowUnconfirmed -and $guard -ne 0)){throw 'Unconfirmed start guard mismatch'}
 $desktop=@($Observed.dwm)
 if(!$desktop.Count){throw 'No DWM observed'}
 foreach($process in $desktop) {
  if(!$process.pid -or !$process.start){throw 'Missing DWM identity'}
  $modules=@($process.modules)
  $cpu=@($modules | Where-Object {$_.name -ieq 'bc250d3d.dll' -and $_.sha256 -eq $Saved.umd_sha256})
  if($cpu.Count -ne 1){throw 'DWM CPU UMD not witnessed'}
  if($null -ne $saveddesktop.modules) {
   # Router desktop: exactly the captured route's module set (router + CPU UMD), nothing more, nothing less.
   $seen=@($modules|ForEach-Object {[string]$_.sha256}|Sort-Object -Unique)
   if(($seen -join ',') -cne ($saveddesktop.modules -join ',') -or $modules.Count -ne $saveddesktop.modules.Count){throw 'Unexpected DWM graphics module'}
   continue
  }
  foreach($module in $modules) {
   if($module.name -ine 'bc250d3d.dll' -or $module.sha256 -ne $Saved.umd_sha256){throw 'Unexpected DWM graphics module'}
  }
 }
}

# Two matching process identities with passing module checks avoid accepting a
# single sample taken while DWM is being replaced. This is not a lifetime promise.
function Wait-KmdCpuBaseline {
 param($Saved,[scriptblock]$Read,[long]$Deadline,[scriptblock]$Record,[scriptblock]$ReadHealth,
       [ValidateRange(1,1000)][int]$IntervalMs=250,[switch]$AllowUnconfirmed)
 $attempts=0;$previous=$null;$firstReady=$null
 while([Diagnostics.Stopwatch]::GetTimestamp() -lt $Deadline){
  $attempts++;$sampleQpc=[Diagnostics.Stopwatch]::GetTimestamp()
  $errorText=$null;$identity=$null;$observed=$null;$health=$null
  try {
   $observed=& $Read
   Assert-KmdCpuBaseline $Saved $observed -AllowUnconfirmed:$AllowUnconfirmed
   if($ReadHealth){
    $health=& $ReadHealth
    if($null -eq $health.generation -or $null -eq $health.epoch -or $health.flags -notin @(7,15)){throw 'Health not ready'}
    Assert-KmdFreshWork $health
   }
   $identity=(@($observed.dwm|ForEach-Object {"$($_.pid):$($_.start)"}|Sort-Object) -join '|')
   if($ReadHealth){$identity+="/health:$($health.generation):$($health.epoch)"}
  } catch {$errorText=[string]$_;$identity=$null}
  $within=[Diagnostics.Stopwatch]::GetTimestamp() -lt $Deadline
  if($identity -and $null -eq $firstReady){$firstReady=$sampleQpc}
  $stable=$within -and $identity -and $identity -eq $previous
  if($Record){& $Record @{attempt=$attempts;qpc=$sampleQpc;identity=$identity;error=$errorText;health=$health;stable=[bool]$stable}|Out-Null}
  if($stable){return @{observed=$observed;health=$health;attempts=$attempts;first_ready_qpc=$firstReady;stable_qpc=[Diagnostics.Stopwatch]::GetTimestamp()}}
  $previous=$identity
  $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())*1000/[double][Diagnostics.Stopwatch]::Frequency
  if($remaining -gt 0){Start-Sleep -Milliseconds ([int][Math]::Min($IntervalMs,[Math]::Ceiling($remaining)))}
 }
 throw "CPU DWM readiness deadline expired after $attempts samples"
}

function Get-KmdReadyHealth {
 param([string]$Text,[string]$Abi)
 $pattern='(?m)^health abi=1 version='+[regex]::Escape($Abi)+' flags=(7|15) generation=([0-9]+) epoch=([0-9]+) completed=([0-9]+) age_ms=([0-9]+) ready_ms=([0-9]+)\r?$'
 $matchesFound=[regex]::Matches($Text,$pattern)
 if($matchesFound.Count -ne 1){throw 'Expected one ready health witness for exact ABI'}
 $m=$matchesFound[0]
 return @{flags=[int]$m.Groups[1].Value;generation=[uint64]$m.Groups[2].Value;epoch=[uint64]$m.Groups[3].Value;completed=[uint64]$m.Groups[4].Value;age_ms=[uint64]$m.Groups[5].Value;ready_ms=[uint64]$m.Groups[6].Value}
}
function Assert-KmdConfirmedHealth {
 param($Before,$After)
 if($After.flags -ne 15 -or $After.generation -ne $Before.generation -or $After.epoch -ne $Before.epoch){throw 'Confirmation does not match observed ready start'}
}

# Mirrors start_health.c confirmation admission (BC250_START_HEALTH_MIN_MS 60000, FRESH_MS 15000; start_health.c
# admission unchanged from 173 13017eab to 174 fbe8e82f; 174 only adds StartHealthIsReady and a
# CU-mode confirm after a successful CONFIRM), not just
# READY flags.
# KMD rechecks this under its lock; a user-mode snapshot cannot replace that check.
function Assert-KmdConfirmEligible {
 param($Health)
 foreach($name in @('flags','completed','age_ms','ready_ms')) {
  if($null -eq $Health.$name){throw "Missing confirmation field: $name"}
 }
 if($Health.flags -notin @(7,15) -or $Health.completed -eq 0 -or
    $Health.ready_ms -lt 60000 -or $Health.age_ms -gt 15000){
  throw 'Health confirmation requires completed work, ready_ms >= 60000 and age_ms <= 15000'
 }
}

function Assert-KmdSameHealthStart {
 param($Before,$After)
 if($After.generation -ne $Before.generation -or $After.epoch -ne $Before.epoch){throw 'Health start changed during verification'}
}
function Assert-KmdFreshWork {
 param($Health)
 if($null -eq $Health.completed -or $Health.completed -eq 0 -or
    $null -eq $Health.age_ms -or $Health.age_ms -gt 15000){throw 'No fresh completed work'}
}
function Wait-KmdConfirmEligible {
 param($Before,[scriptblock]$Read,[long]$Deadline,[scriptblock]$Record,
       [ValidateRange(1,1000)][int]$IntervalMs=500)
 $attempt=0
 while([Diagnostics.Stopwatch]::GetTimestamp() -lt $Deadline){
  $attempt++;$sample=& $Read
  Assert-KmdSameHealthStart $Before $sample
  $eligible=$true
  try{Assert-KmdConfirmEligible $sample}catch{$eligible=$false}
  if($Record){& $Record @{attempt=$attempt;health=$sample;eligible=$eligible;qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|Out-Null}
  if([Diagnostics.Stopwatch]::GetTimestamp() -ge $Deadline){break}
  if($eligible){return $sample}
  $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())*1000/[double][Diagnostics.Stopwatch]::Frequency
  if($remaining -gt 0){Start-Sleep -Milliseconds ([int][Math]::Min($IntervalMs,[Math]::Ceiling($remaining)))}
 }
 throw 'Health confirmation deadline expired; restoration remains unconfirmed'
}
