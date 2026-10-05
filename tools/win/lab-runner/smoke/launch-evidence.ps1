# LAB (elevated SSH, read-only): evidence of a game start since -SinceMinutes: the router's route logs for the image
# (did it open our adapter, which route), Application Error / Windows Error Reporting events naming it, and the
# newest Steam content log lines about its app id (state only).
param([string]$Image = 'ROTTR.exe', [int]$AppId = 391220, [int]$SinceMinutes = 30)
$since = (Get-Date).AddMinutes(-$SinceMinutes)
'--- route logs'
foreach ($root in 'C:\BC250\m14\app-route-001\logs', 'C:\BC250\m15\gpu-dwm-003\logs') {
    if (Test-Path $root) {
        Get-ChildItem $root -Filter "route-$Image-*.log" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $since } |
            ForEach-Object { "{0} {1:HH:mm:ss} lines {2}" -f $_.Name, $_.LastWriteTime.ToUniversalTime(), (Get-Content $_.FullName).Count
                Get-Content $_.FullName | Select-Object -First 2 }
    }
}
'--- application events'
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match [regex]::Escape($Image) -or $_.ProviderName -match 'Application Error|Windows Error Reporting' } |
    Select-Object -First 8 | ForEach-Object {
        "{0:HH:mm:ss}Z {1} {2}: {3}" -f $_.TimeCreated.ToUniversalTime(), $_.ProviderName, $_.Id, (($_.Message -split "`n" | Select-Object -First 6) -join ' | ')
    }
'--- steam content log'
$log = 'C:\Program Files (x86)\Steam\logs\content_log.txt'
if (Test-Path $log) { Get-Content $log -Tail 400 | Where-Object { $_ -match "$AppId" } | Select-Object -Last 12 }
'--- steam console log (app id lines)'
$log = 'C:\Program Files (x86)\Steam\logs\console_log.txt'
if (Test-Path $log) { Get-Content $log -Tail 600 | Where-Object { $_ -match "$AppId|ROTTR|Game process" } | Select-Object -Last 15 }
