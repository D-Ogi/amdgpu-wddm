$ErrorActionPreference='Stop'
# Read-only recovery. Preserve persisted breadcrumbs before potentially slow CIM/PnP queries.
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' |
 Select-Object EnableFullWddm,LastStage,StageHistory,UnconfirmedStarts,KeepLog | Format-List
'registry_read_complete'
$logs=Get-ChildItem C:\BC250\kmdlog -Filter '*.log' | Where-Object { $_.LastWriteTime -ge [datetime]'2026-09-23T17:24:00' } | Sort-Object LastWriteTime
foreach($log in $logs) {
 "snapshot=$($log.Name) bytes=$($log.Length) sha256=$((Get-FileHash -LiteralPath $log.FullName).Hash)"
 Get-Content -LiteralPath $log.FullName
}
Get-ChildItem C:\Windows\Minidump,C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue |
 Select-Object Name,Length,LastWriteTime | Format-Table
'persisted_evidence_read_complete'
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-PnpDevice -Class Display | Select-Object Status,Problem,FriendlyName | Format-Table
'recovery_read_complete'
