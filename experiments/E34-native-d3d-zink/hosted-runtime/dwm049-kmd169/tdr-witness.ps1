function Get-DwmTdrConfiguration {
 $key=Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction Stop
 $values=@{}
 foreach($name in $key.GetValueNames()){
  if($name -like 'Tdr*'){$values[$name]=@{value=$key.GetValue($name);kind=$key.GetValueKind($name).ToString()}}
 }
 return $values
}
function Assert-DwmTdrConfiguration {
 param($Values)
 # Require normal detection/recovery timing, not a deliberately masked timeout.
 $defaults=@{TdrLevel=3;TdrDebugMode=2;TdrDelay=2;TdrDdiDelay=5}
 foreach($name in $defaults.Keys){
  if($Values.ContainsKey($name) -and ($Values[$name].kind -ne 'DWord' -or $Values[$name].value -ne $defaults[$name])){throw "Non-default TDR setting prevents this validation: $name"}
 }
}
function Get-DwmTdrSummary {
 param([string]$Text)
 $blocks=[regex]::Matches($Text,'wddm summary: vsync ')
 if(!$blocks.Count){throw 'Current WDDM summary missing'}
 $tail=$Text.Substring($blocks[$blocks.Count-1].Index)
 $noTdr=[regex]::Matches($tail,'wddm summary: no TDR \(ResetEngine, ResetFromTimeout and RestartFromTimeout were never called\)')
 $debug=[regex]::Matches($tail,'wddm summary: CollectDbgInfo called ([0-9]+) time\(s\) since the driver was loaded')
 if($noTdr.Count -ne 1 -or $debug.Count -ne 1 -or $tail -match '\*\*\* TDR:'){throw 'Missing clean TDR witness or timeout recovery observed'}
 if([uint64]$debug[0].Groups[1].Value -ne 0){throw 'CollectDbgInfo observed'}
 return @{reset_calls=0;collect_debug_calls=0;source='current-complete-summary'}
}
function Get-DwmTdrEvents {
 param([datetime]$Begin,[datetime]$End)
 $result=@()
 foreach($spec in @(@{LogName='System';ProviderName='Display';Id=4101;StartTime=$Begin;EndTime=$End},@{LogName='Application';ProviderName='Windows Error Reporting';Id=1001;StartTime=$Begin;EndTime=$End})){
  try{$events=@(Get-WinEvent -FilterHashtable $spec -ErrorAction Stop)}catch{
   if($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*'){$events=@()}else{throw}
  }
  foreach($event in $events){
   $xml=[xml]$event.ToXml();$values=@($xml.Event.EventData.Data|ForEach-Object {[string]$_.'#text'})
   $suspect=$spec.LogName -eq 'System' -or ($values -contains 'LiveKernelEvent' -and @($values|Where-Object {$_ -in @('117','141')}).Count -gt 0)
   if($suspect){$result+=@{log=$spec.LogName;id=$event.Id;record_id=$event.RecordId;utc=$event.TimeCreated.ToUniversalTime().ToString('o');values=$values}}
  }
 }
 return ,$result
}
