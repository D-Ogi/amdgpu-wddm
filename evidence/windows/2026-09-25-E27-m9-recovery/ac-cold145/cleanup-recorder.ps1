$ErrorActionPreference='Stop'
$task='BC250 cold145 health recorder'
$t=Get-ScheduledTask -TaskName $task
$i=Get-ScheduledTaskInfo -TaskName $task
[pscustomobject]@{State=[string]$t.State;LastRunTime=$i.LastRunTime.ToString('o');LastTaskResult=$i.LastTaskResult} | ConvertTo-Json
if($t.State -ne 'Ready' -or $i.LastTaskResult -ne 0){throw 'Recorder has not completed successfully'}
Unregister-ScheduledTask -TaskName $task -Confirm:$false
'recorder_task_removed'
