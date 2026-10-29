# LAB only. Uses the existing row's unchanged evidence reader and bounded client launch.
param(
  [switch]$Lab,
  [Parameter(Mandatory=$true)][string]$Kit,
  [Parameter(Mandatory=$true)][string]$Out,
  [Parameter(Mandatory=$true)][string]$ExpectSha256,
  [ValidateSet('wait','no-wait','both')][string]$Arm='wait',
  [ValidateRange(10,150)][int]$BoundSeconds=120,
  [string]$Collector='',
  [string]$KmdCli=''
)
$ErrorActionPreference='Stop'
if(-not $Lab){throw 'Pass -Lab on unit A; this wrapper must not inspect the development PC'}
if(-not $Collector){$Collector=Join-Path $PSScriptRoot 'bin\bd215collector.exe'}
if((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count){throw 'Use a fresh empty evidence directory; existing records must not be overwritten'}
$run=Join-Path $Kit 'run.ps1'
$parameters=(Get-Command -Name $run -ErrorAction Stop).Parameters
if(-not $parameters.ContainsKey('Collector')){throw 'Apply client-record.patch and stage the updated run.ps1 before using the collector'}
foreach($name in @('cpu-boot-record.ps1','app-crashes.ps1','state-census.ps1')) {
  if(-not(Test-Path -LiteralPath (Join-Path $PSScriptRoot "lab-tools\$name"))){throw "Missing lab-tools/$name"}
}
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$since=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$tools=Join-Path $PSScriptRoot 'lab-tools'
& (Join-Path $tools 'cpu-boot-record.ps1') -SinceUtc $since | Set-Content -LiteralPath (Join-Path $Out 'cpu-boot-before.txt')
& (Join-Path $tools 'state-census.ps1') -Names 'amdgpu_wddm_d3d1232.dll,amdgpu_wddm_radv_d3d1232.dll' | Set-Content -LiteralPath (Join-Path $Out 'state-before.txt')
if($KmdCli) {
  & $KmdCli cpu | Set-Content -LiteralPath (Join-Path $Out 'cpu-policy-before.txt')
  & $KmdCli dpm | Set-Content -LiteralPath (Join-Path $Out 'dpm-before.txt')
}
& $run -Row hosted-x86 -Arm $Arm -Kit $Kit -Out $Out -ExpectSha256 $ExpectSha256 -Launch session0 -CrashRetries 0 -BoundSeconds $BoundSeconds -Collector $Collector -KmdCli $KmdCli
$rowExit=$LASTEXITCODE
& (Join-Path $tools 'app-crashes.ps1') -SinceUtc $since -Name 'bd201scratch' | Set-Content -LiteralPath (Join-Path $Out 'app-crashes.txt')
& (Join-Path $tools 'cpu-boot-record.ps1') -SinceUtc $since | Set-Content -LiteralPath (Join-Path $Out 'cpu-boot-after.txt')
if($KmdCli) { & $KmdCli cpu | Set-Content -LiteralPath (Join-Path $Out 'cpu-policy-after.txt') }
exit $rowExit
