$ErrorActionPreference='Stop'
. "$PSScriptRoot\tdr-witness.ps1"
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
Assert-DwmTdrConfiguration @{}
Assert-DwmTdrConfiguration @{TdrLevel=@{kind='DWord';value=3};TdrDebugMode=@{kind='DWord';value=2}}
foreach($pair in @(@('TdrLevel',0),@('TdrDebugMode',1),@('TdrDelay',60),@('TdrDdiDelay',60))){$v=@{};$v[$pair[0]]=@{kind='DWord';value=$pair[1]};Must-Reject {Assert-DwmTdrConfiguration $v}}
$t="wddm summary: vsync enabled`nwddm summary: no TDR (ResetEngine, ResetFromTimeout and RestartFromTimeout were never called)`nwddm summary: CollectDbgInfo called 0 time(s) since the driver was loaded"
Get-DwmTdrSummary $t|Out-Null
Must-Reject {Get-DwmTdrSummary ($t -replace 'called 0','called 1')}
Must-Reject {Get-DwmTdrSummary ($t+"`nwddm summary: vsync enabled")}
Must-Reject {Get-DwmTdrSummary ($t+"`nwddm summary: *** TDR:")}
'PASS: masked/changed TDR settings, CollectDbgInfo, truncated newest summary and timeout witness rejection'
function Get-WinEvent {
 param($FilterHashtable,$ErrorAction)
 $xml=if($FilterHashtable.LogName -eq 'System'){'<Event><EventData><Data Name="param1">driver</Data></EventData></Event>'}else{'<Event><EventData><Data Name="EventName">LiveKernelEvent</Data><Data Name="P1">141</Data></EventData></Event>'}
 $e=[pscustomobject]@{Id=$FilterHashtable.Id;RecordId=1;TimeCreated=[datetime]::UtcNow;Xml=$xml}
 $e|Add-Member ScriptMethod ToXml {return $this.Xml}
 return $e
}
$events=Get-DwmTdrEvents ([datetime]::UtcNow.AddMinutes(-1)) ([datetime]::UtcNow)
if($events.Count -ne 2 -or $events[1].values -notcontains 'LiveKernelEvent'){throw 'Event extraction failed'}
function Get-WinEvent {param($FilterHashtable,$ErrorAction);return @()}
$events=Get-DwmTdrEvents ([datetime]::UtcNow.AddMinutes(-1)) ([datetime]::UtcNow)
if($null -eq $events -or $events.Count -ne 0){throw 'Empty event result failed'}
function Get-WinEvent {param($FilterHashtable,$ErrorAction);throw 'Access denied'}
Must-Reject {Get-DwmTdrEvents ([datetime]::UtcNow.AddMinutes(-1)) ([datetime]::UtcNow)}
'PASS: event XML extraction, empty result and failed-query rejection'
