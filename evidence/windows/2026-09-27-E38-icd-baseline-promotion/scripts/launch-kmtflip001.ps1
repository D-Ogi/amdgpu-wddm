$ErrorActionPreference='Stop'
# Launch the kmtflip001 flip control in the interactive session (M546 procedure).
$out='C:\BC250\m13\kmt-flip001'
$task='BC250-KMT-Flip001'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if(Test-Path "$out\done-kmtflip001.json"){throw 'Existing run'}
if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing task'}
if(Test-Path 'C:\BC250\m11\resource-close\bc250d3d.m13-original.dll'){throw 'Unresolved prior UMD backup'}
if(Get-Process runtime-flip-control -ErrorAction SilentlyContinue){throw 'Control still running'}
if(-not (Test-Path "$out\run-kmtflip001.ps1")){throw 'Run script missing'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $out\worker-kmtflip001.ps1"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName,State | ConvertTo-Json
