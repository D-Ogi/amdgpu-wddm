$ErrorActionPreference='Stop'
$out='C:\BC250\e26'
New-Item -ItemType Directory -Force $out | Out-Null
$gpu=Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'PCI\VEN_1002&DEV_13FE*' | Select-Object -First 1
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
$old=(Get-ItemProperty $class -Name UserModeDriverName).UserModeDriverName
$old | Export-Clixml "$out\umd-before.xml"
$script='C:\BC250\tmp\e19_target.ps1'
New-Item -ItemType Directory -Force C:\BC250\e26\umdlogs | Out-Null
icacls C:\BC250\e26\umdlogs /grant '*S-1-1-0:(OI)(CI)M' | Out-Null
try {
 Set-ItemProperty $class -Name UserModeDriverName -Value @('bc250umd.dll','C:\BC250\e26\resident\bc250d3d.dll','C:\BC250\e26\resident\bc250d3d.dll')
 $start=Get-Date
 & $script -Phase gate -Full 1 -GpuVa 1 -Blit 1 -Package C:\BC250\m8 -Tag mesa-resident-probe | Out-File "$out\mesa-resident-gate.txt" -Encoding utf8
 & $script -Phase d3d -Package C:\BC250\m8 -Tag mesa-resident-probe | Tee-Object "$out\mesa-resident-d3d.txt"
 Get-Process dwm | Stop-Process -Force
 Start-Sleep -Seconds 3
 $user=(Get-CimInstance Win32_ComputerSystem).UserName
 if (-not $user) { throw 'No interactive lab user' }
 $action=New-ScheduledTaskAction -Execute 'C:\BC250\e26\render_probe.exe'
 $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
 Register-ScheduledTask -TaskName 'BC250-E26-Render' -Action $action -Principal $principal -Force | Out-Null
 Start-ScheduledTask -TaskName 'BC250-E26-Render'
 Start-Sleep -Seconds 6
 & 'C:\BC250\m8\bc250kmd_cli.exe' fbdump C:\BC250\e26\mesa-resident-active.bmp
 $deadline=(Get-Date).AddSeconds(60)
 do { Start-Sleep -Seconds 2; $state=(Get-ScheduledTask 'BC250-E26-Render').State } while ($state -eq 'Running' -and (Get-Date) -lt $deadline)
 if ($state -eq 'Running') { Stop-ScheduledTask 'BC250-E26-Render'; 'render deadline exceeded' | Out-File "$out\mesa-resident-timeout.txt" }
 Get-ScheduledTaskInfo 'BC250-E26-Render' | Select-Object LastTaskResult | Out-File "$out\mesa-resident-task.txt"
 Get-Content "$out\render-probe.txt"
 Unregister-ScheduledTask 'BC250-E26-Render' -Confirm:$false
 Get-Process dwm | ForEach-Object { "DWM start $($_.StartTime.ToString('s'))" } | Tee-Object "$out\mesa-resident-dwm.txt"
 Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit';StartTime=$start} -ErrorAction SilentlyContinue | ForEach-Object { $_.Message } | Tee-Object "$out\mesa-resident-events.txt"
 & 'C:\BC250\m8\bc250kmd_cli.exe' fbdump C:\BC250\e26\mesa-resident.bmp
 & 'C:\BC250\m8\bc250kmd_cli.exe' log | Out-File "$out\mesa-resident-kmd.txt" -Encoding utf8
} finally {
 Set-ItemProperty $class -Name UserModeDriverName -Value $old
 & $script -Phase gate -Full 0 -Package C:\BC250\m8 -Tag mesa-resident-restored | Out-File "$out\mesa-resident-restored.txt" -Encoding utf8
 & $script -Phase confirm -Package C:\BC250\m8 -Tag mesa-resident-restored
}





