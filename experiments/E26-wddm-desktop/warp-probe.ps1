$ErrorActionPreference='Stop'
$out='C:\BC250\e26'
New-Item -ItemType Directory -Force $out | Out-Null
$gpu=Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'PCI\VEN_1002&DEV_13FE*' | Select-Object -First 1
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
$old=(Get-ItemProperty $class -Name UserModeDriverName).UserModeDriverName
$old | Export-Clixml "$out\umd-before.xml"
$script='C:\BC250\tmp\e19_target.ps1'
try {
 Set-ItemProperty $class -Name UserModeDriverName -Value @('bc250umd.dll','d3d10warp.dll','d3d10warp.dll')
 $start=Get-Date
 & $script -Phase gate -Full 1 -GpuVa 1 -Blit 1 -Package C:\BC250\m8 -Tag warp-probe | Out-File "$out\warp-gate.txt" -Encoding utf8
 & $script -Phase d3d -Package C:\BC250\m8 -Tag warp-probe | Tee-Object "$out\warp-d3d.txt"
 Start-Sleep -Seconds 15
 Get-Process dwm | ForEach-Object { "DWM start $($_.StartTime.ToString('s'))" } | Tee-Object "$out\warp-dwm.txt"
 Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit';StartTime=$start} -ErrorAction SilentlyContinue | ForEach-Object { $_.Message } | Tee-Object "$out\warp-events.txt"
 & 'C:\BC250\m8\bc250kmd_cli.exe' log | Out-File "$out\warp-kmd.txt" -Encoding utf8
} finally {
 Set-ItemProperty $class -Name UserModeDriverName -Value $old
 & $script -Phase gate -Full 0 -Package C:\BC250\m8 -Tag warp-restored | Out-File "$out\warp-restored.txt" -Encoding utf8
 & $script -Phase confirm -Package C:\BC250\m8 -Tag warp-restored
}
