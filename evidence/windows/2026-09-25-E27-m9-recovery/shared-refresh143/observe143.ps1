$ErrorActionPreference='Stop'
$out='C:\BC250\m13\observe143'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
if(Test-Path "$out\samples.txt"){throw 'Output exists'}
& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe info
if($LASTEXITCODE -ne 0){throw 'Driver unavailable'}
'probe_sha256='+(Get-FileHash "$out\driver_observe.exe").Hash
$action=New-ScheduledTaskAction -Execute conhost.exe -Argument "--headless $out\driver_observe.exe $out\samples.txt"
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive session'}
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName BC250-Observe143 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName BC250-Observe143
$deadline=(Get-Date).AddSeconds(45)
try {
 do{Start-Sleep -Seconds 2;$state=(Get-ScheduledTask BC250-Observe143).State}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
 if($state -eq 'Running'){throw 'Raster probe deadline'}
 $code=(Get-ScheduledTaskInfo BC250-Observe143).LastTaskResult
 'raster_exit='+$code
 Get-Content "$out\samples.txt" -Head 4
 Get-Content "$out\samples.txt" -Tail 4
 if($code -ne 0){throw 'Raster API control failed'}
} finally {
 if((Get-ScheduledTask BC250-Observe143).State -eq 'Running'){Stop-ScheduledTask BC250-Observe143}
 Unregister-ScheduledTask BC250-Observe143 -Confirm:$false
}
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'combined_observation_complete='+(Get-Date).ToString('s')
