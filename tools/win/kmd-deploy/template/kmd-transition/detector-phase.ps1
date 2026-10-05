# Bounded child of dispatch Cleanup/RestoreDetector: both hang detector values back as captured, absence included.
# Registry only; the values are read at the next device start, so a running candidate is not affected.
param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidatePattern('^detector-final-[0-9]+$')][string]$Receipt)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\durable.ps1"
. "$PSScriptRoot\hang-detector.ps1"
$Directory=[IO.Path]::GetFullPath($Directory)
if($Directory -notmatch $KmdDirectoryPattern){throw 'Unexpected directory'}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
Write-DurableText "$Directory\$Receipt-start.json" (@{pid=$PID;boot=$boot;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
if(!(Test-Path -LiteralPath "$Directory\baseline.json")){
 # No Capture, so no mutation either: the supervisor writes mutation-start.json only after Capture.
 if(Test-Path -LiteralPath "$Directory\mutation-start.json"){throw 'Mutation without a baseline capture; inspect'}
 Write-DurableText "$Directory\$Receipt-done.json" (@{skipped='no-capture';boot=$boot;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
 exit 0
}
$saved=Get-Content -LiteralPath "$Directory\baseline.json" -Raw|ConvertFrom-Json
Assert-KmdHangDetectorBaseline $saved.hang_detector
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters',$true)
if(!$key){throw 'Driver registry key absent'}
try {$r=Restore-KmdHangDetectorFinal -Key $key -Baseline $saved.hang_detector}finally{$key.Dispose()}
Write-DurableText "$Directory\$Receipt-done.json" (@{changed=$r.changed;before=$r.before;after=$r.after;boot=$boot;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 6)
