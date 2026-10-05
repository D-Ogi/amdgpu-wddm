$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted044'
. "$PSScriptRoot\durable.ps1"
. "$PSScriptRoot\confirmed-present-start.ps1"
. "$PSScriptRoot\request-audit-checkpoint.ps1"
$trialClock=$null
$nextMarker=[UInt64]0
function Save-AuditBoundary([string]$Label) {
 $script:nextMarker++
 $receipt=Request-AuditCheckpoint -MarkerPath "$d\audit-marker.txt" -LogPath "$d\dwm-$gpuPid.log" -Marker $script:nextMarker -ProcessId $gpuPid -ProcessStartUtc $gpuStartUtc -TrialClock $trialClock -DeadlineSeconds 130
 $receipt | Add-Member -NotePropertyName label -NotePropertyValue $Label
 Write-DurableText "$d\boundary-$($script:nextMarker).json" ($receipt | ConvertTo-Json)
}
$traceStarted=$false
$success=$false
$restorationSucceeded=$false
$failure=$null
$gpuPid=0
$measuredSeconds=0
$controlTask='BC250-G0-Composition044'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
Start-Transcript -Path "$d\run.log" -Force | Out-Null
try {
 $freshBaseline=& "$d\preflight.ps1"
 Write-DurableText "$d\preflight.json" ($freshBaseline|Out-String)
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 if((Get-PSDrive C).Free -lt 2GB){throw 'Insufficient capture space'}
 if(Test-Path "$d\started"){throw 'Existing run'}
 if(@(Get-Process witcher3,deqp-vk,vkcube,cross-process-control,cross-process-duplicate-control,gfx-blt-control -ErrorAction SilentlyContinue).Count){throw 'Other lab workload'}
 $health=& $cli health read | Out-String
 if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A6 flags=15'){throw 'KMD166 healthy baseline required'}
 $cfg=Get-Content "$d\manifest.json" -Raw | ConvertFrom-Json
 foreach($property in $cfg.PSObject.Properties){if((Get-FileHash "$d\$($property.Name)").Hash -ne $property.Value){throw "Candidate mismatch: $($property.Name)"}}
 $umd='C:\BC250\m11\resource-close\bc250d3d.dll';$icd='C:\BC250\m10\wsi-final\vulkan_radeon.dll';$standalone='C:\BC250\m13\shared-import001\vulkan_radeon.dll'
 if((Get-FileHash $umd).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected UMD'}
 if((Get-FileHash $icd).Hash -ne 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'){throw 'Unexpected ICD'}
 if((Get-FileHash $standalone).Hash -ne '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'){throw 'Unexpected capability ICD'}
 $raw=(& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe info 2>&1 | Out-String)
 if($LASTEXITCODE -ne 0 -or $raw -notmatch '(?i)VEN_1002&DEV_13FE'){throw 'Adapter check failed'}
 $luidMatches=[regex]::Matches($raw,'(?im)adapter\s+handle\s+0x[0-9a-f]+\s+luid\s+([0-9a-f]{8})-([0-9a-f]{8})')
 if($luidMatches.Count -ne 1){throw 'LUID ambiguous'}
 [IO.File]::WriteAllText("$d\luid.txt",$luidMatches[0].Groups[1].Value+$luidMatches[0].Groups[2].Value,[Text.Encoding]::ASCII)
 Copy-VerifiedDurable $umd "$d\baseline-umd.dll" '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'
 Copy-VerifiedDurable $icd "$d\baseline-icd.dll" 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'
 & "$d\restore.ps1"
 $task='BC250-G0-DwmWatch044'
 if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing watchdog task'}
 $action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d\watchdog.ps1"
 $principal=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
 Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)) | Out-Null
 Start-ScheduledTask -TaskName $task
 Start-Sleep -Milliseconds 700
 if((Get-ScheduledTask -TaskName $task).State -ne 'Running'){throw 'Watchdog not running'}
 for($n=0;$n -lt 50 -and !(Test-Path "$d\watchdog-ready.json");$n++){Start-Sleep -Milliseconds 100}
 if(!(Test-Path "$d\watchdog-ready.json")){throw 'Watchdog readiness missing'}
 $watchReady=Get-Content "$d\watchdog-ready.json" -Raw|ConvertFrom-Json
 $watchProcess=Get-Process -Id $watchReady.pid -ErrorAction Stop
 if($watchProcess.StartTime.ToUniversalTime().ToString('o') -ne $watchReady.start){throw 'Watchdog identity mismatch'}
 Write-DurableText "$d\started" ([DateTime]::UtcNow.ToString('o'))
 if(Get-ScheduledTask -TaskName $controlTask -ErrorAction SilentlyContinue){throw 'Existing control task'}
 $who=(Get-CimInstance Win32_ComputerSystem).UserName
 if(!$who){throw 'No interactive user'}
 $controlAction=New-ScheduledTaskAction -Execute "$d\composition-control.exe"
 $controlPrincipal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
 Register-ScheduledTask -TaskName $controlTask -Action $controlAction -Principal $controlPrincipal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)) | Out-Null
 Start-ScheduledTask -TaskName $controlTask
 & "$d\wait-stable-composition.ps1" -Directory $d -TaskName $controlTask
 if(!(Test-Path "$d\baseline-ready.json")){throw 'CPU baseline readiness witness missing'}
 & logman start BC250G0Dwm044 -ets -o "$d\gpu.etl" -pf "$d\etw-providers.txt" -f bin -bs 1024 -nb 64 256 *> "$d\etw-start.log"
 if($LASTEXITCODE -ne 0){throw 'ETW start failed'}
 $traceStarted=$true

 $trialQpc=[Diagnostics.Stopwatch]::GetTimestamp()
 $trialClock=[Diagnostics.Stopwatch]::StartNew()
 Write-DurableText "$d\trial-boundary.json" (@{utc=[DateTime]::UtcNow.ToString('o');qpc=$trialQpc;qpc_frequency=[Diagnostics.Stopwatch]::Frequency;rollback_seconds=140;acceptance_seconds=180}|ConvertTo-Json)
 try {
  $installMutex=New-Object Threading.Mutex($false,'Global\BC250G0DwmRestore044')
  $installLocked=$false
  try {
   try {$installLocked=$installMutex.WaitOne(15000)} catch [Threading.AbandonedMutexException] {$installLocked=$true}
   if(!$installLocked){throw 'Installation mutex timeout'}
   if(Test-Path "$d\abort"){throw 'Watchdog aborted trial'}
  Move-Item -LiteralPath $umd -Destination "$d\original-umd.dll"
  Copy-VerifiedDurable "$d\router.dll" $umd $cfg.'router.dll'
  Move-Item -LiteralPath $icd -Destination "$d\original-icd.dll"
  Copy-VerifiedDurable $standalone $icd '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'
  # Preserve early root/fence records before the ordinary log ring wraps.
  $collector=Start-Process -FilePath "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\collect-startup.ps1") -WindowStyle Hidden -PassThru
  @{pid=$collector.Id;start=$collector.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content "$d\collector-start.json"
  Flush-ExistingFile "$d\collector-start.json"
  & "$d\interop.ps1" -Value 1
  # PnP restart may replace the adapter LUID. Publish it before allowing the
  # one-process router claim; no DWM started during restart can consume it.
  $raw=(& $cli info 2>&1 | Out-String)
  if($LASTEXITCODE -ne 0){throw 'Post-transition adapter query failed'}
  $luidMatches=[regex]::Matches($raw,'(?im)adapter\s+handle\s+0x[0-9a-f]+\s+luid\s+([0-9a-f]{8})-([0-9a-f]{8})')
  if($luidMatches.Count -ne 1){throw 'Post-transition LUID ambiguous'}
  [IO.File]::WriteAllText("$d\luid.txt",$luidMatches[0].Groups[1].Value+$luidMatches[0].Groups[2].Value,[Text.Encoding]::ASCII)
  [IO.File]::WriteAllText("$d\enable",'bounded DWM route')
  $before=@(Get-Process dwm | Select-Object -ExpandProperty Id)
  "DWM before=$($before -join ',')"
  $restartUtc=[DateTime]::UtcNow
  Write-DurableText "$d\restart-boundary.json" (@{utc=$restartUtc.ToString('o');previous_pids=$before} | ConvertTo-Json)
  foreach($idValue in $before){Stop-Process -Id $idValue -Force}
  } finally {if($installLocked){$installMutex.ReleaseMutex()};$installMutex.Dispose()}
  $ready=& "$d\wait-hosted-startup.ps1" -Directory $d -PreviousPids $before -TaskName $controlTask -RestartUtc $restartUtc
  $gpuPid=[int]$ready.sample.pid
  $gpuStartUtc=(Get-Process -Id $gpuPid).StartTime.ToUniversalTime()
  Write-DurableText "$d\animate" ([DateTime]::UtcNow.ToString('o'))
  $confirmed=$false
  $healthGeneration=[UInt64]0;$healthEpoch=[UInt64]0
  $watch=[Diagnostics.Stopwatch]::StartNew()
  $i=0
  $lastLogBytes=0
  $lastProgress=0
  Save-AuditBoundary "render-start"
  while($trialClock.Elapsed.TotalSeconds -lt 105) {
   Start-Sleep -Seconds 5
   $procs=@(Get-Process dwm)
   if($procs.Count -ne 1){throw 'Expected one DWM process'}
   $proc=$procs[0]
   if($proc.Id -ne $gpuPid){throw 'Unexpected DWM restart'}
   $mods=@($proc.Modules | Where-Object {$_.ModuleName -match 'bc250|vulkan_radeon'} | ForEach-Object {@{name=$_.ModuleName;path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
   @{utc=[DateTime]::UtcNow.ToString('o');elapsed=$watch.Elapsed.TotalSeconds;pid=$proc.Id;modules=@($proc.Modules | ForEach-Object {@{name=$_.ModuleName;path=$_.FileName}});candidate_modules=$mods;claim_exists=(Test-Path "$d\claim");enable_exists=(Test-Path "$d\enable")} | ConvertTo-Json -Depth 6 | Set-Content "$d\startup-$i-$gpuPid.json"
   if(!($mods | Where-Object {$_.sha256 -eq $cfg.'bc250d3d_zink.dll'})){throw 'GPU UMD witness missing'}
   if(!($mods | Where-Object {$_.sha256 -eq $cfg.'vulkan_radeon.dll'})){throw 'Hosted ICD witness missing'}
   $log="$d\dwm-$gpuPid.log"
   if(!(Test-Path $log)){throw 'DWM log missing'}
   $logBytes=(Get-Item $log).Length
   if($logBytes -gt $lastLogBytes){$lastProgress=$watch.Elapsed.TotalSeconds}
   $lastLogBytes=$logBytes
   if($watch.Elapsed.TotalSeconds - $lastProgress -gt 30){throw 'No diagnostic log growth for30 seconds'}
   $thermal=(& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 2>&1 | Out-String)
   if($LASTEXITCODE -ne 0 -or $thermal -notmatch 'Tctl\s+([0-9.]+)\s+C'){throw 'Thermal witness unavailable'}
   $temperature=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
   if($temperature -gt 85){throw 'Thermal limit'}
   if((Get-PSDrive C).Free -lt 1GB){throw 'Capture space limit'}
   if((Get-ScheduledTask -TaskName $controlTask).State -ne 'Running'){throw 'Composition control ended early'}
   if(!$confirmed) {
    $healthRaw=& $cli health read | Out-String
    $healthCode=$LASTEXITCODE
    $healthRaw | Set-Content "$d\health-wait-$i.txt"
    if($healthCode -ne 0){throw 'Health query failed'}
    $decision=Get-ConfirmedPresentStart -Health $healthRaw -ElapsedSeconds $watch.Elapsed.TotalSeconds -ExpectedGeneration $healthGeneration -ExpectedEpoch $healthEpoch
    $healthGeneration=$decision.generation;$healthEpoch=$decision.epoch
    if($decision.launch){Write-DurableText "$d\confirmed-health.json" ($decision | ConvertTo-Json);$confirmed=$true}
   }
   $measuredSeconds=$watch.Elapsed.TotalSeconds
   @{utc=[DateTime]::UtcNow.ToString('o');elapsed=$measuredSeconds;pid=$proc.Id;cpu_seconds=$proc.CPU;temperature_c=$temperature;modules=$mods;log_bytes=(Get-Item $log).Length} | ConvertTo-Json -Depth 5 | Set-Content "$d\process-$i-$gpuPid.json"
   if($i -eq 0){
    & $cli log summary *> "$d\kmd-start.log"
    if($LASTEXITCODE -ne 0){throw 'Startup GPU Present summary unavailable'}
    $summary=Get-Content "$d\kmd-start.log" -Raw
    $admission=[regex]::Matches($summary,'GPU Present submits([0-9]+) rejected([0-9]+) failed([0-9]+)')
    if(!$admission.Count){throw 'GPU Present admission counters unavailable'}
    $latest=$admission[$admission.Count-1]
    if([long]$latest.Groups[2].Value -gt 0 -or [long]$latest.Groups[3].Value -gt 0){throw 'GPU Present rejected or failed; rollback with original diagnostics'}
    if([long]$latest.Groups[1].Value -eq 0){throw 'No GPU Present submission witness; rollback'}
   }
   if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
   if($i -in 2,8){
    Save-AuditBoundary "dynamic-$i-capture-start"
    & $cli fbdump "$d\dynamic-$i.bmp" *> "$d\dynamic-$i-dump.log"
    if($LASTEXITCODE -ne 0){throw 'Dynamic primary dump failed'}
    Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$d\dynamic-$i.png" -TimeoutSec 5
    Save-AuditBoundary "dynamic-$i-capture-end"
   }
   if(Test-Path "$d\control-heartbeat.json"){
    $heartbeat=Get-Content "$d\control-heartbeat.json" -Raw | ConvertFrom-Json
    if(!$heartbeat.animating -or $heartbeat.frames -lt 1){throw 'Animation not witnessed'}
    if($heartbeat.qpc_frequency -le 0 -or $heartbeat.qpc_after -lt $heartbeat.qpc_before){throw 'Invalid clock anchor'}
    if($heartbeat.paint_red -lt 1 -or $heartbeat.paint_blue -lt 1 -or $heartbeat.paint_moving -lt 1){throw 'Missing WM_PAINT witness'}
    $heartbeat | ConvertTo-Json | Set-Content "$d\control-progress-$i.json"
   }
   $i++
  }
  Save-AuditBoundary 'render-end'
  if(!$confirmed){throw 'Health never confirmed'}
  # Freeze only after the measured animation interval; never serialize each draw.
  [IO.File]::WriteAllText("$d\freeze",[DateTime]::UtcNow.ToString('o'))
  $settled=$false
  for($attempt=0;$attempt -lt 50;$attempt++){
   Start-Sleep -Milliseconds 100
   $state=Get-Content "$d\control-heartbeat.json" -Raw | ConvertFrom-Json
   if($state.frozen -eq $true -and $state.freeze_result -eq 0){
    $state | ConvertTo-Json -Depth 5 | Set-Content "$d\freeze-receipt.json"
    $settled=$true;break
   }
  }
  if(!$settled){throw 'Final control freeze/DwmFlush not acknowledged'}
  & $cli log summary *> "$d\kmd-end.log"
  Save-AuditBoundary "final-capture-start"
  & $cli fbdump "$d\gpu.bmp" *> "$d\gpu-dump.log"
  if($LASTEXITCODE -ne 0){throw 'GPU primary dump failed'}
  Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$d\screen.png" -TimeoutSec 5
  Save-AuditBoundary "final-capture-end"
  foreach($image in 'gpu.bmp','screen.png'){
   $check=& "$d\check-stable-composition.ps1" -Image "$d\$image" -Heartbeat "$d\freeze-receipt.json" -Frozen
   $check | ConvertTo-Json -Depth 6 | Set-Content "$d\$image-check.json"
   if(!$check.pass){throw "Final full-client composition failed: $image"}
  }
  $success=$true

 } finally {
  & "$d\restore.ps1" -Restart
  $restorationSucceeded=$true
  if($trialClock.Elapsed.TotalSeconds -gt 180){$success=$false;throw 'Overall180-second budget exceeded'}
 }
} catch {$success=$false;$failure=$_|Out-String;$failure;throw} finally {
 if($traceStarted){& logman stop BC250G0Dwm044 -ets *> "$d\etw-stop.log"}
 Get-ScheduledTask -TaskName $controlTask -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
 Get-Process composition-control -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq "$d\composition-control.exe"} | Stop-Process -Force -ErrorAction SilentlyContinue
 Write-DurableText "$d\done.json" (@{utc=[DateTime]::UtcNow.ToString('o');success=$success;restoration_succeeded=$restorationSucceeded;failure=$failure;gpu_pid=$gpuPid;measured_seconds=$measuredSeconds;trial_seconds=$(if($trialClock){$trialClock.Elapsed.TotalSeconds}else{0});markers=$nextMarker} | ConvertTo-Json)
 Stop-Transcript | Out-Null
}
