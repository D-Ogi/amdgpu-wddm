# The driver Parameters a postflight compares with the attempt's capture, and the ones it only reports.
#
# The KMD itself writes some values at every start or while it runs: the start guard, the stage record, the
# log-keep status, and the outcome of the CU mode and DPM decisions. They describe the last start, so they differ
# from a capture whenever a start ended differently (CuModeLastReason captured 6 after a cold-boot
# refusal, 0 after the candidate's start applied 24). Such a difference is telemetry, not a changed setting: it is
# reported, never a failure. Everything else in Parameters is an operator or installer setting and must equal the
# capture, absence included. A value that exists live but not in the capture (a new INF default, a guard mark) is
# reported too; the gates and guards that matter have their own checks in postflight.ps1.
function Get-KmdWrittenParameterNames {
 @('UnconfirmedStarts','LastStage','StageHistory','KeepStatus',   # guard.c, start guard and stage record
   'CuModeLastApplied','CuModeLastReason',                          # cumode.c CuModeFinish, every start
   'DpmLastMode','DpmLastReason','DpmSession')                      # dpm.c, every start / while above the floor
}

# The GPU DWM interop record the KMD keeps beside the operator's two switches (driver\kmd\interop.c since 0.7.181):
# written at every full start (LastState, LastReason), at a session mark and unmark (Session, LastEnd) and when the
# driver closes the switches itself (ClosedReason). Not in the generic list above: a promotion over a CPU desktop
# with the switches 0 never saw them change. Configure never writes them back; postflight checks the latch itself.
function Get-KmdInteropWrittenNames {
 @('InteropLastState','InteropLastReason','InteropSession','InteropClosedReason','InteropLastEnd')
}

function Test-KmdParameterValueEqual($A,$B){
 if($A -is [array] -or $B -is [array]){return (@($A) -join "`n") -ceq (@($B) -join "`n")}
 return $A -eq $B
}

# Saved: the capture's parameters (name -> {value, kind}, from JSON). Live: name -> @{value; kind}.
# Skip: names with their own check (the hang detector's). Returns differs (operator values that are missing or
# changed: a failure), kmd_written (captured/live of every KMD-written value, whether it changed) and added.
function Compare-KmdCapturedParameters {
 param([Parameter(Mandatory)]$Saved,[Parameter(Mandatory)][Collections.IDictionary]$Live,[string[]]$Skip=@())
 $written=Get-KmdWrittenParameterNames
 $captured=[ordered]@{}
 foreach($p in @($Saved.PSObject.Properties)){$captured[$p.Name]=$p.Value}
 $r=[ordered]@{differs=@();kmd_written=@();added=@()}
 foreach($name in $captured.Keys){
  if($name -in $Skip){continue}
  $present=$Live.Contains($name)
  if($name -in $written){
   $r.kmd_written+=[ordered]@{name=$name;captured=$captured[$name].value;live=$(if($present){$Live[$name].value}else{$null});changed=!$present -or !(Test-KmdParameterValueEqual $captured[$name].value $Live[$name].value)}
   continue
  }
  if(!$present -or !(Test-KmdParameterValueEqual $captured[$name].value $Live[$name].value)){$r.differs+=$name}
 }
 foreach($name in @($Live.Keys)){
  if($captured.Contains($name) -or $name -in $Skip){continue}
  if($name -in $written){$r.kmd_written+=[ordered]@{name=$name;captured=$null;live=$Live[$name].value;changed=$true}}
  else{$r.added+=[ordered]@{name=$name;live=$Live[$name].value}}
 }
 $r
}
