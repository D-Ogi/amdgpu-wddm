# Test-only hang bugcheck, new in driver revision 172 (driver/kmd/hang.c, 6420bb68): <service key>\Parameters,
#   EnableHangBugcheck  REG_DWORD, 1 arms a deliberate bugcheck at the next device start; absent or 0 is closed.
#                       The 173 and 174 INFs write 0 on every install.
#   HangBugcheckSeconds REG_DWORD 5..60, inert unless armed; no INF writes it.
# Both are read only at device start. This promotion never arms the detector: the baseline must be closed, the
# candidate starts with an explicit 0, and the restore arm and Cleanup put both values back as captured, absence
# included. Key is a Microsoft.Win32.RegistryKey or a test double with the same five methods.
function Get-KmdHangDetectorNames {return @('EnableHangBugcheck','HangBugcheckSeconds')}
function Read-KmdHangDetector {
 param([Parameter(Mandatory)]$Key)
 $present=@($Key.GetValueNames())
 $state=[ordered]@{}
 foreach($name in Get-KmdHangDetectorNames){
  if($present -contains $name){
   $kind=[string]$Key.GetValueKind($name)
   $value=if($kind -eq 'DWord'){[int]$Key.GetValue($name)}else{[string]$Key.GetValue($name)}
   $state[$name]=@{present=$true;kind=$kind;value=$value}
  }else{$state[$name]=@{present=$false;kind=$null;value=$null}}
 }
 return $state
}
# Normalizes a receipt (JSON round trip) or a fresh read; anything else is malformed, never "absent".
function ConvertTo-KmdHangDetectorState {
 param($State)
 if($null -eq $State){throw 'Missing hang detector state'}
 $result=[ordered]@{}
 foreach($name in Get-KmdHangDetectorNames){
  $entry=if($State -is [Collections.IDictionary]){$State[$name]}else{$State.$name}
  if($null -eq $entry -or $entry.present -isnot [bool]){throw "Malformed hang detector state: $name"}
  if(!$entry.present){$result[$name]=@{present=$false;kind=$null;value=$null};continue}
  if($entry.kind -cne 'DWord' -or $null -eq $entry.value -or $entry.value -isnot [ValueType]){throw "Unsupported hang detector value: $name"}
  $result[$name]=@{present=$true;kind='DWord';value=[int]$entry.value}
 }
 return $result
}
function Assert-KmdHangDetectorBaseline {
 param($State)
 $s=ConvertTo-KmdHangDetectorState $State
 if($s.EnableHangBugcheck.present -and $s.EnableHangBugcheck.value -ne 0){throw 'Hang detector armed in the baseline; disarm deliberately before this promotion'}
}
function Assert-KmdHangDetectorClosed {
 param($State)
 $s=ConvertTo-KmdHangDetectorState $State
 if($s.EnableHangBugcheck.present -and $s.EnableHangBugcheck.value -ne 0){throw 'Hang detector armed'}
}
# The candidate's start state: explicitly closed, HangBugcheckSeconds as captured.
function Get-KmdCandidateHangDetector {
 param($Baseline)
 $b=ConvertTo-KmdHangDetectorState $Baseline
 return [ordered]@{EnableHangBugcheck=@{present=$true;kind='DWord';value=0};HangBugcheckSeconds=$b.HangBugcheckSeconds}
}
function Test-KmdHangDetectorEqual {
 param($A,$B)
 $x=ConvertTo-KmdHangDetectorState $A;$y=ConvertTo-KmdHangDetectorState $B
 foreach($name in Get-KmdHangDetectorNames){
  if($x[$name].present -ne $y[$name].present){return $false}
  if($x[$name].present -and $x[$name].value -ne $y[$name].value){return $false}
 }
 return $true
}
function Set-KmdHangDetector {
 param([Parameter(Mandatory)]$Key,[Parameter(Mandatory)]$Desired)
 $want=ConvertTo-KmdHangDetectorState $Desired
 $before=Read-KmdHangDetector $Key
 foreach($name in Get-KmdHangDetectorNames){
  if($want[$name].present){$Key.SetValue($name,[int]$want[$name].value,[Microsoft.Win32.RegistryValueKind]::DWord)}
  else{$Key.DeleteValue($name,$false)}
 }
 $Key.Flush()
 $after=Read-KmdHangDetector $Key
 if(!(Test-KmdHangDetectorEqual $after $want)){throw 'Hang detector readback mismatch'}
 return @{before=$before;desired=$want;after=$after}
}
# Cleanup: the value is either as captured or as this attempt's candidate left it. A third state was written by
# someone else; it is preserved and reported, like a changed SetupAPI log level.
function Restore-KmdHangDetectorFinal {
 param([Parameter(Mandatory)]$Key,[Parameter(Mandatory)]$Baseline)
 $base=ConvertTo-KmdHangDetectorState $Baseline
 $current=Read-KmdHangDetector $Key
 if(Test-KmdHangDetectorEqual $current $base){return @{changed=$false;before=$current;after=$current}}
 if(!(Test-KmdHangDetectorEqual $current (Get-KmdCandidateHangDetector $base))){throw 'Hang detector changed outside this attempt; preserved, inspect'}
 $r=Set-KmdHangDetector -Key $Key -Desired $base
 return @{changed=$true;before=$r.before;after=$r.after}
}
