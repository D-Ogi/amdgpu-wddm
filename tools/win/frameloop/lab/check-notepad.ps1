# Lists GUI processes the broken frameloop .cmd wrapper may have opened on the console desktop (2026-10-02 quick runs).
Get-Process | Where-Object { $_.MainWindowTitle -match 'smoke|g12-l7|g0-l7|frameloop' -or $_.ProcessName -match '^notepad$|frameloop' } |
    Select-Object Id, ProcessName, SessionId, StartTime, MainWindowTitle | Format-Table -AutoSize | Out-String -Width 200
