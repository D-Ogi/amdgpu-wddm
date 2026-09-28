. "$PSScriptRoot\common.ps1"
$code=125;$wrapper=$null;$mutated=$false;$captured=$false
try {
 Assert-Stage
 if((Get-Process -Id $PID).SessionId -eq 0){throw 'Interactive session required'}
 Assert-StopThermal
 if(Test-Path "$d\done.json"){throw 'Existing run'}
 if(@(Get-Control).Count){throw 'Audit control already live'}
 $ready=Get-Content "$d\watchdog-ready.json" -Raw|ConvertFrom-Json
 $watcher=Get-Process -Id $ready.pid -ErrorAction Stop
 if($watcher.StartTime.ToUniversalTime().ToString('o') -ne $ready.start -or (Get-ScheduledTask $watchdogTask).State -ne 'Running'){throw 'No live matching watchdog'}
 $preflight=(& "$d\preflight.ps1"|Out-String)
 Write-DurableText "$d\preflight.json" $preflight
 $before=$preflight|ConvertFrom-Json
 foreach($item in $items){
  if((Get-FileHash -LiteralPath $item.source).Hash -ne $item.candidate){throw 'Candidate source mismatch'}
  Copy-VerifiedDurable $item.path $item.backup $item.baseline
 }
 if(Test-Path "$d\abort"){throw 'Watchdog abort before mutation'}
 $installMutex=New-Object Threading.Mutex($false,'Global\BC250WindowClient001Restore')
 $installLocked=$false
 try {
  try{$installLocked=$installMutex.WaitOne(15000)}catch [Threading.AbandonedMutexException]{$installLocked=$true}
  if(!$installLocked){throw 'Install mutex timeout'}
  if(Test-Path "$d\abort"){throw 'Watchdog abort under install lock'}
  Write-DurableText "$d\enable" 'audit client only'
  $mutated=$true
  foreach($item in $items){
  if(Test-Path "$d\abort"){throw 'Watchdog abort during mutation'}
  if((Get-FileHash $item.path).Hash -ne $item.baseline){throw 'Baseline changed before rename'}
  Move-Item -LiteralPath $item.path -Destination $item.original
  Copy-VerifiedDurable $item.source $item.path $item.candidate
  }
 if(Test-Path "$d\abort"){throw 'Watchdog abort before process start'}
 $env:BC250_D3D_RUNTIME_PROBE='1';$env:BC250_HOSTED_RENDER='1';$env:BC250_HOST_AUDIT='1';$env:BC250_UPLOAD_AUDIT='1'
 $env:BC250_HOSTED_ICD="$d\vulkan_radeon.dll"
 $env:MESA_SHADER_CACHE_DISABLE='true';$env:MESA_SHADER_CACHE_DIR="$d\cache";$env:MESA_LOG_FILE="$d\mesa.log"
 $commandArguments='/d /s /c ""{0}" "{1}" 1>"{3}" 2>"{2}""' -f "$d\gpu-window-control.exe","$d","$d\stderr.log","$d\stdout.log"
 $wrapper=Start-Process -FilePath "$env:windir\System32\cmd.exe" -ArgumentList $commandArguments -WorkingDirectory $d -WindowStyle Hidden -PassThru
 Write-DurableText "$d\wrapper.json" (@{pid=$wrapper.Id;start=$wrapper.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json)
 }finally{if($installLocked){$installMutex.ReleaseMutex()};$installMutex.Dispose()}
 $watch=[Diagnostics.Stopwatch]::StartNew();$sampled=$false;$lastCheck=0
 while(!$wrapper.WaitForExit(100)){
  if($watch.Elapsed.TotalSeconds -ge 45 -or (Test-Path "$d\abort")){throw 'Control deadline/abort'}
  if($watch.Elapsed.TotalSeconds - $lastCheck -ge 2){Assert-StopThermal;$lastCheck=$watch.Elapsed.TotalSeconds}
  if($sampled -and !$captured -and (Test-Path "$d\heartbeat.json")){
   $state=Get-Content "$d\heartbeat.json" -Raw|ConvertFrom-Json
   if($state.pid -ne $childIdentity){throw 'Heartbeat PID mismatch'}
   if($state.frames -ge 30 -and !(Test-Path "$d\freeze")){Write-DurableText "$d\freeze" 'freeze for capture'}
   if($state.frozen -eq $true){
    $captureStart=[Diagnostics.Stopwatch]::GetTimestamp()
    if($state.qpc -gt $captureStart -or $state.frequency -ne [Diagnostics.Stopwatch]::Frequency){throw 'Invalid frozen clock anchor'}
    Write-DurableText "$d\freeze-before.json" ($state|ConvertTo-Json -Depth 5)
    & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe fbdump "$d\gpu.bmp" *> "$d\gpu-dump.log"
    if($LASTEXITCODE -ne 0){throw 'Primary capture failed'}
    Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$d\screen.png" -TimeoutSec 5
    $captureEnd=[Diagnostics.Stopwatch]::GetTimestamp()
    $after=Get-Content "$d\heartbeat.json" -Raw|ConvertFrom-Json
    if($after.pid -ne $state.pid -or !$after.frozen -or $after.frames -ne $state.frames -or (($after.client -join ',') -ne ($state.client -join ','))){throw 'Frozen geometry changed during capture'}
    if(!(Get-Control|Where-Object {$_.Id -eq $state.pid})){throw 'Client exited during capture'}
    Write-DurableText "$d\capture.json" (@{start_qpc=$captureStart;end_qpc=$captureEnd;frequency=[Diagnostics.Stopwatch]::Frequency;before=$state;after=$after}|ConvertTo-Json -Depth 5)
    $captured=$true;Write-DurableText "$d\stop" 'capture complete'
   }
  }
  if(!$sampled){
   foreach($child in @(Get-Control)){
    $mods=@($child.Modules|Where-Object {$_.ModuleName -in @('bc250d3d_zink.dll','vulkan_radeon.dll')}|ForEach-Object {@{name=$_.ModuleName;path=$_.FileName;sha256=(Get-FileHash -LiteralPath $_.FileName).Hash}})
    if(@($mods|Where-Object {$_.path -eq "$d\bc250d3d_zink.dll"}).Count -eq 1 -and @($mods|Where-Object {$_.path -eq "$d\vulkan_radeon.dll"}).Count -eq 1){
     if(@($mods|Where-Object {$_.path -eq "$d\bc250d3d_zink.dll" -and $_.sha256 -eq $cfg.'bc250d3d_zink.dll'}).Count -ne 1 -or @($mods|Where-Object {$_.path -eq "$d\vulkan_radeon.dll" -and $_.sha256 -eq $cfg.'vulkan_radeon.dll'}).Count -ne 1){throw 'Loaded candidate identity mismatch'}
     Write-DurableText "$d\modules.json" (@{pid=$child.Id;start=$child.StartTime.ToUniversalTime().ToString('o');modules=$mods}|ConvertTo-Json -Depth 5)
     $childIdentity=$child.Id;$sampled=$true
    }
   }
  }
 }
 $wrapper.Refresh();$code=$wrapper.ExitCode
 if($code -ne 0){throw "Control exit $code"}
 if(!$sampled){throw 'No loaded module witness'}
 if(!$captured){throw 'No ordered frozen captures'}
 if((Get-Content "$d\stdout.log" -Raw) -notmatch 'PASS gpu window frames='){throw 'Missing control result'}
 $after=@(Get-Process dwm|ForEach-Object {@{pid=$_.Id;start=$_.StartTime.ToString('o')}})
 $beforeIdentity=($before.dwm|ForEach-Object {"$($_.pid):$($_.start)"}|Sort-Object) -join ','
 $afterIdentity=($after|ForEach-Object {"$($_.pid):$($_.start)"}|Sort-Object) -join ','
 if($beforeIdentity -ne $afterIdentity){throw 'DWM identity changed'}
 Write-DurableText "$d\control-result.json" (@{exit=$code;utc=[DateTime]::UtcNow.ToString('o');dwm=$after}|ConvertTo-Json -Depth 4)
}catch{$code=125;$_|Out-String|Add-Content "$d\worker.log"}
finally{
 try{
  if($mutated){& "$d\restore.ps1"}
  if($wrapper -and !$wrapper.HasExited){$wrapper.Kill();if(!$wrapper.WaitForExit(5000)){throw 'Wrapper remains live'}}
  if($mutated){
   $closureText=((& "$d\preflight.ps1")|Out-String)
   Write-DurableText "$d\closure.json" $closureText
   $closure=$closureText|ConvertFrom-Json
   $beforeIdentity=($before.dwm|ForEach-Object {"$($_.pid):$($_.start)"}|Sort-Object) -join ','
   $closureIdentity=($closure.dwm|ForEach-Object {"$($_.pid):$($_.start)"}|Sort-Object) -join ','
   if($beforeIdentity -ne $closureIdentity -or $before.boot -ne $closure.boot -or $before.confirmed.generation -ne $closure.confirmed.generation -or $before.confirmed.epoch -ne $closure.confirmed.epoch){throw 'DWM/boot/health identity changed at closure'}
  }
 }catch{$code=126;$_|Out-String|Add-Content "$d\worker.log"}
 Write-DurableText "$d\done.json" (@{exit=$code;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
}
exit $code
