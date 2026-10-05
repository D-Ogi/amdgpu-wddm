$ErrorActionPreference='Stop'
$out='C:\BC250\m10\cts-smoke-04'
if(Test-Path "$out\result.txt"){throw 'Existing results'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
if((Get-FileHash 'C:\BC250\m10\wsi-fifo\vulkan_radeon.dll').Hash -ne '3A03A1729F678A492D22220702E37A5BB4F3F9C2D23DE747075A139E20875F72'){throw 'ICD hash mismatch'}
$clock=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe clock read 2>&1 | Out-String
$clock | Set-Content "$out\clock.log"
if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
Copy-Item -LiteralPath 'C:\BC250\m8\vulkan-1.dll' -Destination "$out\vulkan-1.dll"
Get-FileHash "$out\vulkan-1.dll" | Format-List
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive session'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $out\worker.ps1"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 12)
Register-ScheduledTask -TaskName 'BC250-M10-CtsSmoke04' -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask 'BC250-M10-CtsSmoke04'
'CTS_TASK_STARTED'
