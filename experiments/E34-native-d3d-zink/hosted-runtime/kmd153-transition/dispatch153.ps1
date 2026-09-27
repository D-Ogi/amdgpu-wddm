param([ValidateSet('Prepare','Start','Inspect','Cleanup')][string]$Mode='Inspect')
$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07153'
$name='BC250 KMD153 diagnostic transition'
if($Mode -eq 'Prepare'){
 if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){throw 'Task already exists; inspect it'}
 if(Test-Path (Join-Path $out 'worker-start.json')){throw 'Existing worker evidence; do not rerun'}
 $hashes=Get-Content (Join-Path $out 'staging-hashes.json') -Raw | ConvertFrom-Json
 foreach($file in $hashes.PSObject.Properties){
  if((Get-FileHash -LiteralPath (Join-Path $out $file.Name)).Hash -ne $file.Value){throw ('Staging hash mismatch: '+$file.Name)}
 }
 $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\BC250\m12\candidate07153\worker153.ps1'
 $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
 $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -MultipleInstances IgnoreNew
 Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
}
if($Mode -eq 'Start'){
 if(Test-Path (Join-Path $out 'worker-start.json')){throw 'Existing job must be inspected, never restarted'}
 $task=Get-ScheduledTask -TaskName $name
 if($task.State -ne 'Ready'){throw 'Task not ready'}
 if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP requested'}
 $hashes=Get-Content (Join-Path $out 'staging-hashes.json') -Raw | ConvertFrom-Json
 foreach($file in $hashes.PSObject.Properties){
  if((Get-FileHash -LiteralPath (Join-Path $out $file.Name)).Hash -ne $file.Value){throw ('Staging hash mismatch: '+$file.Name)}
 }
 Start-ScheduledTask -TaskName $name
}
$task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
$info=if($task){Get-ScheduledTaskInfo -TaskName $name}else{$null}
$state=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');exists=($null -ne $task);state=if($task){[string]$task.State}else{'Missing'};result=if($info){$info.LastTaskResult}else{$null};boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
foreach($kind in @('worker','collector')){
 $p=Join-Path $out ($kind+'-start.json')
 if(Test-Path $p){
  $receipt=Get-Content $p -Raw | ConvertFrom-Json
  $process=Get-Process -Id $receipt.pid -ErrorAction SilentlyContinue
  $state[$kind+'_alive']=($null -ne $process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $receipt.start)
  $state[$kind+'_start']=$receipt
 }
 $p=Join-Path $out ($kind+'-done.json')
 if(Test-Path $p){$state[$kind+'_done']=Get-Content $p -Raw | ConvertFrom-Json}
}
if($Mode -eq 'Cleanup'){
 if($state.state -eq 'Running' -or $state.worker_alive -or $state.collector_alive){throw 'Job still active'}
 if($task){Unregister-ScheduledTask -TaskName $name -Confirm:$false}
 $state.removed=$true
}
$state | ConvertTo-Json -Depth 6
