param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$d=$Out
if(Test-Path $d){throw 'Test output already exists'}
New-Item -ItemType Directory $d | Out-Null
Copy-Item "$PSScriptRoot\dwm030-gpu-present\durable.ps1" "$d\durable.ps1"
Copy-Item "$PSScriptRoot\hosted-startup-witness.ps1" "$d\hosted-startup-witness.ps1"
$actual=Get-Content "$PSScriptRoot\wait-hosted-startup.ps1" -Raw
$body=$actual.Substring($actual.IndexOf("`$ErrorActionPreference='Stop'"))
[IO.File]::WriteAllText("$d\actual-wrapper.ps1",'param([string]$Directory,[int[]]$PreviousPids,[string]$TaskName)'+[Environment]::NewLine+$body)
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
function global:Get-Process {param($Name) if($Name -ne 'dwm'){throw 'Unexpected process query'};return [pscustomobject]@{Id=5608;StartTime=[DateTime]'2026-09-27T12:00:00Z';Modules=$global:fakeModules}}
$result=& "$d\actual-wrapper.ps1" -Directory $d -PreviousPids @(2016) -TaskName 'mock'
if(!$result.ready -or $result.sample.pid -ne 5608 -or !(Test-Path "$d\hosted-ready.json")){throw 'Wrapper did not preserve readiness'}
'Real wrapper with mocked external observations PASS'
