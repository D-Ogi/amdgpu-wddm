$name = 'Lab-Present-Heartbeat'
$t = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
if ($t) { if ($t.State -eq 'Running') { Stop-ScheduledTask -TaskName $name }; Unregister-ScheduledTask -TaskName $name -Confirm:$false; "removed (was $($t.State))" } else { 'absent' }
