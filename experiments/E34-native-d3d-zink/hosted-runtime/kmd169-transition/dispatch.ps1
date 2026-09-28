param([ValidateSet('Prepare','Start','Inspect','Cleanup')][string]$Mode='Inspect',
 [Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$ManifestSha256)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-stage.ps1"
. "$PSScriptRoot\durable.ps1"
$Directory=[IO.Path]::GetFullPath($Directory)
if($Directory -notmatch '^C:\\BC250\\m13\\kmd169-[a-z0-9-]+$'){throw 'Unexpected directory'}
$name='BC250-KMD169-Watch'
$exe="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
$arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$Directory+'\kmd169-transition\launch.ps1" -Directory "'+$Directory+'" -ManifestSha256 '+$ManifestSha256
if($Mode -in @('Prepare','Start')){
 [void](Assert-KmdStage $Directory $ManifestSha256)
 if(Test-Path "$Directory\start-requested.json"){throw 'Already requested; inspect, never restart'}
 if(Test-Path "$Directory\boundary.json"){throw 'Existing attempt; inspect, never restart'}
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
}
if($Mode -eq 'Prepare'){
 if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){throw 'Existing task; inspect it'}
 $action=New-ScheduledTaskAction -Execute $exe -Argument $arguments
 $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
 $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 180) -MultipleInstances IgnoreNew
 Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings|Out-Null
}
if($Mode -eq 'Start'){
 $task=Get-ScheduledTask -TaskName $name
 if($task.State -ne 'Ready' -or @($task.Actions).Count -ne 1 -or
  $task.Actions[0].Execute -ine $exe -or $task.Actions[0].Arguments -cne $arguments -or
  $task.Principal.UserId -notin @('SYSTEM','S-1-5-18') -or
  $task.Settings.ExecutionTimeLimit -ne 'PT3M'){throw 'Task admission mismatch'}
 Write-DurableText "$Directory\start-requested.json" (@{utc=[DateTime]::UtcNow.ToString('o');manifest=$ManifestSha256}|ConvertTo-Json)
 Start-ScheduledTask -TaskName $name
}
$task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
$info=if($task){Get-ScheduledTaskInfo -TaskName $name}else{$null}
$result=$null
if(Test-Path "$Directory\watch-result.json"){$result=Get-Content "$Directory\watch-result.json" -Raw|ConvertFrom-Json}
if($Mode -eq 'Cleanup'){
 if($task -and $task.State -eq 'Running'){throw 'Task still running'}
 if(!$result -or $result.status -notin @('closed','cancelled')){throw 'No verified terminal result; inspect recovery'}
 if($task){Unregister-ScheduledTask -TaskName $name -Confirm:$false}
}
@{utc=[DateTime]::UtcNow.ToString('o');exists=($null -ne $task);
 state=if($task){[string]$task.State}else{'Missing'};
 task_result=if($info){$info.LastTaskResult}else{$null};result=$result;cleanup=($Mode -eq 'Cleanup')}|ConvertTo-Json -Depth 8
