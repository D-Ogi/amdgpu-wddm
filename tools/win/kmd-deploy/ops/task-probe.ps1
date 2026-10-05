# Read-only diagnostic child: postflight.ps1 line 52 verbatim, sampled for a few seconds, plus every Running task
# with its path, so a task that exists only while a bounded child runs shows up.
param([int]$Seconds = 8)
$ErrorActionPreference = 'Stop'
$end = [DateTime]::UtcNow.AddSeconds($Seconds)
$seen = [ordered]@{}
while ([DateTime]::UtcNow -lt $end) {
    $result = [ordered]@{}
    $result.running_tasks = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' } | Select-Object TaskName,State)
    foreach ($t in $result.running_tasks) { $k = 'filter: [' + $t.TaskName + '] ' + $t.State + ' ' + $t.GetType().FullName; if (!$seen.Contains($k)) { $seen[$k] = [DateTime]::UtcNow.ToString('HH:mm:ss.fff') } }
    $bad = @($result.running_tasks | Where-Object { $_.TaskName -notin @('BC250 monitor overlay','BC250 net watchdog') })
    foreach ($t in $bad) { $k = 'LINE80 would throw on: [' + $t.TaskName + '] len ' + ([string]$t.TaskName).Length; if (!$seen.Contains($k)) { $seen[$k] = [DateTime]::UtcNow.ToString('HH:mm:ss.fff') } }
    foreach ($t in @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' })) { $k = 'running: ' + $t.TaskPath + $t.TaskName; if (!$seen.Contains($k)) { $seen[$k] = [DateTime]::UtcNow.ToString('HH:mm:ss.fff') } }
    Start-Sleep -Milliseconds 250
}
'PID ' + $PID + ' parent ' + (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
$seen.GetEnumerator() | ForEach-Object { $_.Value + '  ' + $_.Key }
