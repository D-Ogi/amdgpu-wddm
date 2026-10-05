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
 Set-ItemProperty $class -Name UserModeDriverName -Value @('bc250umd.dll','C:\BC250\e26\bc250d3d.dll','C:\BC250\e26\bc250d3d.dll')
 $start=Get-Date
 & $script -Phase gate -Full 1 -GpuVa 1 -Blit 1 -Package C:\BC250\m8 -Tag mesa-fresh-probe | Out-File "$out\mesa-fresh-gate.txt" -Encoding utf8
 & $script -Phase d3d -Package C:\BC250\m8 -Tag mesa-fresh-probe | Tee-Object "$out\mesa-fresh-d3d.txt"
 Get-Process dwm | Stop-Process -Force
 Start-Sleep -Seconds 45
 Get-Process dwm | ForEach-Object { "DWM start $($_.StartTime.ToString('s'))" } | Tee-Object "$out\mesa-fresh-dwm.txt"
 Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit';StartTime=$start} -ErrorAction SilentlyContinue | ForEach-Object { $_.Message } | Tee-Object "$out\mesa-fresh-events.txt"
 & 'C:\BC250\m8\bc250kmd_cli.exe' fbdump C:\BC250\e26\mesa-fresh.bmp
 & 'C:\BC250\m8\bc250kmd_cli.exe' log | Out-File "$out\mesa-fresh-kmd.txt" -Encoding utf8
} finally {
 Set-ItemProperty $class -Name UserModeDriverName -Value $old
 & $script -Phase gate -Full 0 -Package C:\BC250\m8 -Tag mesa-fresh-restored | Out-File "$out\mesa-fresh-restored.txt" -Encoding utf8
 & $script -Phase confirm -Package C:\BC250\m8 -Tag mesa-fresh-restored
}

