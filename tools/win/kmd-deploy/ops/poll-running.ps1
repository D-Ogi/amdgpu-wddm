foreach ($i in 1..8) {
    $r = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' -and $_.TaskName -notin @('BC250 monitor overlay','BC250 net watchdog') })
    if ($r.Count) { $r | ForEach-Object { "$([DateTime]::UtcNow.ToString('HH:mm:ss.f')) $($_.TaskPath)$($_.TaskName)" } }
    Start-Sleep -Milliseconds 250
}
'done'
