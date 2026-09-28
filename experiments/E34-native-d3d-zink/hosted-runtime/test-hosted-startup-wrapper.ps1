param([Parameter(Mandatory)][string]$Out,[ValidateSet("ready","no-process","module-retry","old-overlap","ambiguous")][string]$Mode="ready")
$ErrorActionPreference='Stop'
$d=$Out
if(Test-Path $d){throw 'Test output already exists'}
New-Item -ItemType Directory $d | Out-Null
Copy-Item "$PSScriptRoot\dwm030-gpu-present\durable.ps1" "$d\durable.ps1"
Copy-Item "$PSScriptRoot\hosted-startup-witness.ps1" "$d\hosted-startup-witness.ps1"
$actual=Get-Content "$PSScriptRoot\wait-hosted-startup.ps1" -Raw
$body=$actual.Substring($actual.IndexOf("`$ErrorActionPreference='Stop'"))
[IO.File]::WriteAllText("$d\actual-wrapper.ps1",'param([string]$Directory,[int[]]$PreviousPids,[string]$TaskName,[datetime]$RestartUtc)'+[Environment]::NewLine+$body)
$hashes=@{};$global:fakeModules=@()
foreach($n in 'router.dll','bc250d3d_zink.dll','vulkan_radeon.dll'){
 [IO.File]::WriteAllText("$d\$n",$n)
 $hashes[$n]=(Get-FileHash "$d\$n").Hash
 $global:fakeModules+= [pscustomobject]@{ModuleName=if($n -eq 'router.dll'){'bc250d3d.dll'}else{$n};FileName="$d\$n"}
}
$hashes | ConvertTo-Json | Set-Content "$d\manifest.json"
[IO.File]::WriteAllText("$d\dwm-5608.log",'DWM CreateDevice hr=00000000')
function global:Invoke-RestMethod {param($Uri,$TimeoutSec) return @{stop=$false}}
function global:Get-ScheduledTask {param($TaskName) return @{State='Running'}}
$global:sampleMode=$Mode;$global:processReads=0
function global:Get-Process {
 [CmdletBinding()]param($Name)
 if($Name -ne 'dwm'){throw 'Unexpected process query'}
 $global:processReads++
 if($global:sampleMode -eq 'no-process' -and $global:processReads -eq 1){
  # Real cmdlet's missing-process error must be suppressed by the caller.
  return Microsoft.PowerShell.Management\Get-Process -Name 'BC250-NoSuchProcess-WrapperTest' -ErrorAction $ErrorActionPreference
 }
 $p=[pscustomobject]@{Id=5608;StartTime=[DateTime]'2026-09-27T12:00:00Z'}
 $p | Add-Member -MemberType ScriptProperty -Name Modules -Value {
  if($global:sampleMode -eq 'module-retry' -and $global:processReads -eq 1){throw [ComponentModel.Win32Exception]::new(299)}
  return $global:fakeModules
 }
 if($global:sampleMode -in 'old-overlap','ambiguous'){
  $start=if($global:sampleMode -eq 'old-overlap'){[datetime]'2026-09-27T11:58:00Z'}else{[datetime]'2026-09-27T12:00:01Z'}
  return @($p,[pscustomobject]@{Id=5609;StartTime=$start})
 }
 return $p
}
# Reproduce a live UMD writer: ReadAllText must fail, shared read must pass.
$writer=[IO.File]::Open("$d\dwm-5608.log",[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
try {
 $oldReaderRejected=$false
 try {[IO.File]::ReadAllText("$d\dwm-5608.log") | Out-Null} catch [IO.IOException] {$oldReaderRejected=$true}
 if(!$oldReaderRejected){throw 'Negative control did not reproduce writer sharing conflict'}
 $failure=$null
 try {$result=& "$d\actual-wrapper.ps1" -Directory $d -PreviousPids @(2016) -TaskName 'mock' -RestartUtc ([datetime]'2026-09-27T11:59:00Z')} catch {$failure=$_}
 if($Mode -eq 'ambiguous'){
  if(!$failure -or "$failure" -notmatch 'Ambiguous new DWM identities'){throw 'Ambiguity not rejected'}
  $receipt=Get-Content "$d\hosted-startup-001.json" -Raw | ConvertFrom-Json
  if(!$receipt.sample.ambiguous -or $receipt.sample.candidates.Count -ne 2 -or $receipt.ready -or (Test-Path "$d\hosted-ready.json")){throw 'Ambiguity receipt missing or accepted'}
  'Ambiguous identities rejected with durable PID/start receipt PASS';return
 }
 if($failure){throw $failure}
} finally {$writer.Dispose()}
if(!$result.ready -or $result.sample.pid -ne 5608 -or !(Test-Path "$d\hosted-ready.json")){throw 'Wrapper did not preserve readiness'}
if($Mode -in 'no-process','module-retry' -and $global:processReads -lt 2){throw 'Transient condition did not retry'}
if(!$result.first_create_success_utc){throw 'Missing first creation observation time'}
"Real wrapper with live log writer: $Mode PASS"
