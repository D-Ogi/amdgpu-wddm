$ErrorActionPreference='Stop'
$out='C:\BC250\m13\menu-control'
New-Item -ItemType Directory -Force $out | Out-Null
$user=(Get-CimInstance Win32_ComputerSystem).UserName
$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\BC250\tmp\menu2-interactive.ps1'
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName 'BC250-M13-Menu' -Action $action -Principal $principal -Force | Out-Null
Get-Process StartMenuExperienceHost -ErrorAction SilentlyContinue | Stop-Process -Force
Start-ScheduledTask -TaskName 'BC250-M13-Menu'
Start-Sleep -Seconds 9
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\menu2.bmp"
Start-Sleep -Seconds 8
Get-ScheduledTaskInfo 'BC250-M13-Menu' | Select-Object LastTaskResult | Format-List
Get-Content "$out\keys2.txt"
Unregister-ScheduledTask 'BC250-M13-Menu' -Confirm:$false
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\closed2.bmp"
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
'menu_control_complete'
