# Lab-only entry points. Call inside a bounded phase, with durable Save callback.
function Get-KmdSetupLogLevel {
 $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Setup')
 if(!$key){throw 'Setup registry key missing'}
 try {
  $present=$key.GetValueNames() -contains 'LogLevel'
  if($present -and $key.GetValueKind('LogLevel') -ne [Microsoft.Win32.RegistryValueKind]::DWord){throw 'Unexpected Setup LogLevel kind'}
  return @{present=$present;value=$(if($present){[int]$key.GetValue('LogLevel')}else{$null})}
 }finally{$key.Dispose()}
}
function Set-KmdSetupLogLevel {
 param($State)
 if($State.present -isnot [bool]){throw 'Malformed logging state'}
 $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Setup',$true)
 if(!$key){throw 'Setup registry key missing'}
 try {
  if($State.present){
   if($null -eq $State.value){throw 'Missing logging value'}
   $key.SetValue('LogLevel',[int]$State.value,[Microsoft.Win32.RegistryValueKind]::DWord)
  }else{$key.DeleteValue('LogLevel',$false)}
  $key.Flush()
 }finally{$key.Dispose()}
}
function Test-KmdSetupLogState {
 param($A,$B)
 return ($A.present -is [bool] -and $B.present -is [bool] -and $A.present -eq $B.present -and (!$A.present -or $A.value -eq $B.value))
}
function Start-KmdSetupLog {
 param([scriptblock]$Read={Get-KmdSetupLogLevel},[scriptblock]$Write={param($s) Set-KmdSetupLogLevel $s},[Parameter(Mandatory)][scriptblock]$Save)
 $before=& $Read
 if($before.present -isnot [bool] -or ($before.present -and $null -eq $before.value)){throw 'Malformed prior log state'}
 & $Save $before|Out-Null
 $verbose=@{present=$true;value=65535}
 & $Write $verbose|Out-Null
 if(!(Test-KmdSetupLogState (& $Read) $verbose)){throw 'Verbose logging readback failed; use saved state for recovery'}
 return $before
}
function Restore-KmdSetupLog {
 param($Before,[scriptblock]$Read={Get-KmdSetupLogLevel},[scriptblock]$Write={param($s) Set-KmdSetupLogLevel $s})
 $current=& $Read
 if(Test-KmdSetupLogState $current $Before){return}
 if(!(Test-KmdSetupLogState $current @{present=$true;value=65535})){throw 'Logging value changed outside this trial; preserve it and report conflict'}
 & $Write $Before|Out-Null
 if(!(Test-KmdSetupLogState (& $Read) $Before)){throw 'Logging restore readback failed'}
}
function Get-KmdSetupLogOffsets {
 $result=@{}
 foreach($name in @('setupapi.dev.log','setupapi.app.log')){
  $path=Join-Path "$env:windir\INF" $name
  if(Test-Path -LiteralPath $path){
   $item=Get-Item -LiteralPath $path -ErrorAction Stop
   $result[$name]=@{present=$true;bytes=$item.Length;last_write_utc=$item.LastWriteTimeUtc.ToString('o')}
  }else{$result[$name]=@{present=$false;bytes=$null;last_write_utc=$null}}
 }
 return @{utc=[DateTime]::UtcNow.ToString('o');logs=$result}
}
