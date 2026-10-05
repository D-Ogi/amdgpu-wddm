# Status of gdipromo002: task state, done file, log tail; unregisters the task once it is not running.
$dir = 'C:\BC250\m13\icd-promotion002'
$task = 'BC250-M13-gdipromo002'
"utc " + [DateTime]::UtcNow.ToString('o')
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
"task " + $(if ($t) { $t.State } else { 'absent' })
"done " + (Test-Path "$dir\done-gdipromo002.json")
if (Test-Path "$dir\done-gdipromo002.json") { Get-Content "$dir\done-gdipromo002.json" }
"vkcube " + ((Get-Process vkcube -ErrorAction SilentlyContinue | Measure-Object).Count)
if (Test-Path "$dir\run-gdipromo002.log") { Get-Content "$dir\run-gdipromo002.log" | Where-Object { $_ -notmatch '^t=' } | Select-Object -Last 40 }
if ($t -and $t.State -ne 'Running' -and (Test-Path "$dir\done-gdipromo002.json")) { Unregister-ScheduledTask -TaskName $task -Confirm:$false; 'task unregistered' }
