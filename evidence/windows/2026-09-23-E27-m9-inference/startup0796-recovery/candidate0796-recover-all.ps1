$ErrorActionPreference='Stop'
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' |
 Select-Object EnableFullWddm,LastStage,StageHistory,UnconfirmedStarts,KeepStatus | Format-List
Get-PnpDevice -Class Display | Select-Object Status,Problem,FriendlyName | Format-Table
$logs=Get-ChildItem C:\BC250\kmdlog -Filter '*.log' | Where-Object { $_.LastWriteTime -ge [datetime]'2026-09-23T12:47:00' } | Sort-Object LastWriteTime
foreach($log in $logs) {
 "snapshot=$($log.Name) bytes=$($log.Length) sha256=$((Get-FileHash -LiteralPath $log.FullName).Hash)"
 Get-Content -LiteralPath $log.FullName
}
Get-ChildItem C:\Windows\Minidump,C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue |
 Select-Object Name,Length,LastWriteTime | Format-Table
'recovery_read_complete'
