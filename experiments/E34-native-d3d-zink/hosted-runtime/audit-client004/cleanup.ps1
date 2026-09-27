. "$PSScriptRoot\common.ps1"
if(!(Test-Path "$d\done.json") -or !(Test-Path "$d\watchdog-done.json")){throw 'Both workers must be terminal'}
if((Get-Content "$d\watchdog-done.json" -Raw|ConvertFrom-Json).exit -ne 0){throw 'Watchdog restoration not successful'}
if(@(Get-Control).Count){throw 'Control remains live'}
foreach($item in $items){if((Get-FileHash $item.path).Hash -ne $item.baseline){throw 'Baseline mismatch before cleanup'}}
foreach($name in @($workerTask,$watchdogTask)){
 $task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
 if($task){
  if($task.State -eq 'Running'){throw 'Task still running'}
  if(@($task.Actions|Where-Object {$_.Arguments -like "* -File $d\*"}).Count -ne 1){throw 'Task action mismatch'}
  Unregister-ScheduledTask -TaskName $name -Confirm:$false
 }
}
Write-DurableText "$d\closed.json" (@{utc=[DateTime]::UtcNow.ToString('o');done=(Get-Content "$d\done.json" -Raw|ConvertFrom-Json);umd=(Get-FileHash $items[0].path).Hash;icd=(Get-FileHash $items[1].path).Hash}|ConvertTo-Json -Depth 5)
