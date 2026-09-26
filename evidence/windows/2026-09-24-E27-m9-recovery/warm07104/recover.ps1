$ErrorActionPreference='Stop'
# Read-only acquisition. Do not start services, reset DWM, change gates or query GPU escapes.
function Read-Section([string]$Name,[scriptblock]$Action) {
    "BEGIN $Name utc=$([datetime]::UtcNow.ToString('o'))"
    try { & $Action } catch { "ERROR $Name $($_.Exception.Message)" }
    "END $Name utc=$([datetime]::UtcNow.ToString('o'))"
}
function Read-LogSnapshot([string]$Path) {
    $before=Get-Item -LiteralPath $Path
    "FILE $($before.Name) bytes=$($before.Length) modified=$($before.LastWriteTimeUtc.ToString('o'))"
    $hashBefore=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    Get-Content -LiteralPath $Path
    $after=Get-Item -LiteralPath $Path
    $hashAfter=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    "FILE_END $($before.Name) sha256_before=$hashBefore sha256_after=$hashAfter bytes_after=$($after.Length) stable=$($hashBefore -eq $hashAfter)"
}
Read-Section 'registry' {
    Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' |
        Select-Object EnableFullWddm,LastStage,StageHistory,UnconfirmedStarts,KeepLog | Format-List
}
Read-Section 'persisted_startup_logs' {
    $logs=Get-ChildItem -LiteralPath 'C:\BC250\kmdlog' -Filter '*.log' |
        Where-Object { $_.LastWriteTime -ge [datetime]'2026-09-24T02:14:00' } | Sort-Object LastWriteTime
    foreach($log in $logs) { Read-Section $log.Name { Read-LogSnapshot $log.FullName } }
}
foreach($name in @('before.log')) {
    $path=Join-Path 'C:\BC250\m9\warm07104' $name
    Read-Section $name { Read-LogSnapshot $path }
}
Read-Section 'dump_metadata' {
    Get-ChildItem C:\Windows\Minidump,C:\Windows\MEMORY.DMP -ErrorAction SilentlyContinue |
        Select-Object Name,Length,LastWriteTimeUtc | Format-Table
}
'persisted_evidence_read_complete'
# Potentially slow device-management queries run only after persistent evidence.
Read-Section 'boot' { 'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s') }
Read-Section 'display_device' { Get-PnpDevice -Class Display | Select-Object Status,Problem,FriendlyName | Format-Table }
'recovery_read_complete'
