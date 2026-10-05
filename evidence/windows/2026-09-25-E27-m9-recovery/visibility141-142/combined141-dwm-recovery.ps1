$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
$out='C:\BC250\m9\candidate07141'
& C:\BC250\m8\bc250kmd_cli.exe log summary | Tee-Object "$out\before-dwm-recovery.log"
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\before-dwm-recovery.bmp"
if($LASTEXITCODE -ne 0){throw 'Scanout capture failed'}
Get-Process dwm | Select-Object Id,StartTime,CPU,WorkingSet64 | Format-List
'restart_dwm='+(Get-Date).ToString('s')
Get-Process dwm | Stop-Process -Force
Start-Sleep -Seconds 8
Get-Process dwm | Select-Object Id,StartTime,CPU,WorkingSet64 | Format-List
& C:\BC250\m8\bc250kmd_cli.exe log summary | Tee-Object "$out\after-dwm-recovery.log"
'recovery_check='+(Get-Date).ToString('s')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
