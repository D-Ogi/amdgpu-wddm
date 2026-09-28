$ErrorActionPreference = 'Stop'
if (Get-Process witcher3 -ErrorAction SilentlyContinue) { throw 'witcher3 running' }
$t = Get-ScheduledTask -TaskName 'BC250-M12-witcher3-dx12-010' -ErrorAction SilentlyContinue
if ($t) { "state=$($t.State)"; if ($t.State -eq 'Running') { throw 'task running' }; Unregister-ScheduledTask -TaskName 'BC250-M12-witcher3-dx12-010' -Confirm:$false; 'task_unregistered' } else { 'no task' }
"done=" + (Test-Path 'C:\BC250\m12\witcher3-dx12\done-w3dx12-010.json')
