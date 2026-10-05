# Launch run 005 (candidate ICD 93B1D1FD: E14 smoke + feature level probe) in the interactive session.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\fl-probe001'
$task = 'BC250-E37-FL005'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if (Test-Path "$dir\done-fl005.json") { throw 'Existing run' }
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { throw 'Existing task' }
if (Test-Path "$dir\baseline-9C40083C.dll") { throw 'Unresolved prior ICD backup' }
if (Get-Process fl-probe -ErrorAction SilentlyContinue) { throw 'Probe still running' }
if (-not (Test-Path "$dir\candidate-kmt-enum\vulkan_radeon.dll")) { throw 'Candidate missing' }
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) { throw 'No interactive user' }
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker-fl005.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | ConvertTo-Json
