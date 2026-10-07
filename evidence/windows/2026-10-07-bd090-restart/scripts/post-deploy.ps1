# LAB: state after the 0.7.216.3 deploy: boot time, driver, device status, guard values, log tail.
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
$d = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
"device $($d.Status) problem $($d.Problem) driver $((Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data)"
$p = Get-ItemProperty $par
"UnconfirmedStarts $($p.UnconfirmedStarts) KeepLog $($p.KeepLog) DpmMaxMHz $($p.DpmMaxMHz) CuMode $($p.CuMode) StageHistory $($p.StageHistory)"
$g = Get-ItemProperty "$par\GuardBoot" -ErrorAction SilentlyContinue
"GuardBoot Confirmed $($g.Confirmed)"
Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { "dwm pid $($_.Id) start $($_.StartTime.ToUniversalTime().ToString('o'))" }
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddMinutes(-30); Id=1074,6005,6006,6008,41} -ErrorAction SilentlyContinue | Select-Object -First 8 | ForEach-Object { '{0:o} {1} {2}' -f $_.TimeCreated.ToUniversalTime(), $_.Id, ($_.Message -split "`n")[0] }
