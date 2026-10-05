# Best-effort fields are explicitly tagged; unreadable is never zero/success.
# Runs inside the phase's bounded job, including CIM/property queries.
function Read-KmdInstallField {
 param([scriptblock]$Read)
 try {
  $value=& $Read
  if($null -eq $value){throw 'No value returned'}
  return @{readable=$true;value=$value;error=$null}
 }catch{return @{readable=$false;value=$null;error=[string]$_}}
}
function Get-KmdInstallObservation {
 param([string]$Instance)
 $fields=@{}
 foreach($item in @(
  @('version','DEVPKEY_Device_DriverVersion'),
  @('problem','DEVPKEY_Device_ProblemCode'),
  @('published_inf','DEVPKEY_Device_DriverInfPath'),
  @('driver_key','DEVPKEY_Device_Driver'))){
  $key=$item[1]
  $fields[$item[0]]=Read-KmdInstallField {(Get-PnpDeviceProperty -InstanceId $Instance -KeyName $key -ErrorAction Stop).Data}
 }
 $fields.service=Read-KmdInstallField {
  $service=@(Get-CimInstance Win32_SystemDriver -Filter "Name='bc250kmd'" -ErrorAction Stop)
  if($service.Count -ne 1){throw 'Expected one KMD service'}
  @{state=$service[0].State;started=$service[0].Started;path=$service[0].PathName}
 }
 return @{utc=[DateTime]::UtcNow.ToString('o');qpc=[Diagnostics.Stopwatch]::GetTimestamp();fields=$fields}
}
# Save the post-call observation before interpreting a native failure exit.
# A phase killed at its outer deadline may have only the before receipt.
function Invoke-KmdObservedInstall {
 param([scriptblock]$Read,[scriptblock]$Save,[scriptblock]$Install)
 $before=& $Read
 & $Save 'before' $before|Out-Null
 if(!$before.fields.service.readable -or $before.fields.service.value.state -ne 'Stopped'){
  throw 'Deferred installation requires an observed stopped KMD service'
 }
 $failure=$null;$exitCode=$null
 try {$exitCode=& $Install}catch{$failure=$_}
 $after=& $Read
 & $Save 'after' @{native_exit=$exitCode;invocation_error=$(if($failure){[string]$failure}else{$null});observation=$after}|Out-Null
 if($failure){throw $failure}
 if($null -eq $exitCode -or $exitCode -is [array] -or $exitCode -ne 0){throw "Deferred install failed (exit $exitCode); inspect before/after receipts"}
 return $after
}
