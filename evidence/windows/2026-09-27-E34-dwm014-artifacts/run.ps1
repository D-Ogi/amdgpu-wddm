$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted014'
$traceStarted=$false
$success=$false
$gpuPid=0
$measuredSeconds=0
$controlTask='BC250-G0-Composition014'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
Start-Transcript -Path "$d\run.log" -Force | Out-Null
try {
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 if((Get-PSDrive C).Free -lt 2GB){throw 'Insufficient capture space'}
 if(Test-Path "$d\started"){throw 'Existing run'}
 $cfg=Get-Content "$d\manifest.json" -Raw | ConvertFrom-Json
 foreach($property in $cfg.PSObject.Properties){if((Get-FileHash "$d\$($property.Name)").Hash -ne $property.Value){throw "Candidate mismatch: $($property.Name)"}}
 $umd='C:\BC250\m11\resource-close\bc250d3d.dll';$icd='C:\BC250\m10\wsi-final\vulkan_radeon.dll';$standalone='C:\BC250\m13\shared-import001\vulkan_radeon.dll'
 if((Get-FileHash $umd).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected UMD'}
 if((Get-FileHash $icd).Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'){throw 'Unexpected ICD'}
 if((Get-FileHash $standalone).Hash -ne '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'){throw 'Unexpected capability ICD'}
 $raw=(& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe info 2>&1 | Out-String)
 if($LASTEXITCODE -ne 0 -or $raw -notmatch '(?i)VEN_1002&DEV_13FE'){throw 'Adapter check failed'}
 $luidMatches=[regex]::Matches($raw,'(?im)adapter\s+handle\s+0x[0-9a-f]+\s+luid\s+([0-9a-f]{8})-([0-9a-f]{8})')
 if($luidMatches.Count -ne 1){throw 'LUID ambiguous'}
 [IO.File]::WriteAllText("$d\luid.txt",$luidMatches[0].Groups[1].Value+$luidMatches[0].Groups[2].Value,[Text.Encoding]::ASCII)
 Copy-Item -LiteralPath $umd -Destination "$d\baseline-umd.dll"
 Copy-Item -LiteralPath $icd -Destination "$d\baseline-icd.dll"
 & "$d\restore.ps1"
 $task='BC250-G0-DwmWatch014'
 if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing watchdog task'}
 $action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d\watchdog.ps1"
 $principal=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
 Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)) | Out-Null
 Start-ScheduledTask -TaskName $task
 Start-Sleep -Milliseconds 700
 if((Get-ScheduledTask -TaskName $task).State -ne 'Running'){throw 'Watchdog not running'}
 [IO.File]::WriteAllText("$d\started",[DateTime]::UtcNow.ToString('o'))
 if(Get-ScheduledTask -TaskName $controlTask -ErrorAction SilentlyContinue){throw 'Existing control task'}
 $who=(Get-CimInstance Win32_ComputerSystem).UserName
 if(!$who){throw 'No interactive user'}
 $controlAction=New-ScheduledTaskAction -Execute "$d\composition-control.exe"
 $controlPrincipal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
 Register-ScheduledTask -TaskName $controlTask -Action $controlAction -Principal $controlPrincipal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)) | Out-Null
 Start-ScheduledTask -TaskName $controlTask
 Start-Sleep -Seconds 2
 & $cli fbdump "$d\baseline.bmp" *> "$d\baseline-dump.log"
 if($LASTEXITCODE -ne 0){throw 'Baseline dump failed'}
 & logman start BC250G0Dwm014 -ets -o "$d\gpu.etl" -p Microsoft-Windows-DxgKrnl 0xffffffffffffffff 5 -f bin -bs 1024 -nb 64 256 *> "$d\etw-start.log"
 if($LASTEXITCODE -ne 0){throw 'ETW start failed'}
 $traceStarted=$true

 try {
  Move-Item -LiteralPath $umd -Destination "$d\original-umd.dll"
  Copy-Item -LiteralPath "$d\router.dll" -Destination $umd
  Move-Item -LiteralPath $icd -Destination "$d\original-icd.dll"
  Copy-Item -LiteralPath $standalone -Destination $icd
  [IO.File]::WriteAllText("$d\enable",'bounded DWM route')
  $before=@(Get-Process dwm | Select-Object -ExpandProperty Id)
  "DWM before=$($before -join ',')"
  foreach($idValue in $before){Stop-Process -Id $idValue -Force}
  $watch=[Diagnostics.Stopwatch]::StartNew()
  $i=0
  $lastLogBytes=0
  $lastProgress=0
  while($watch.Elapsed.TotalSeconds -lt 180) {
   Start-Sleep -Seconds 5
   $procs=@(Get-Process dwm)
   if($procs.Count -ne 1){throw 'Expected one DWM process'}
   $proc=$procs[0]
   if(!$gpuPid){$gpuPid=$proc.Id}
   if($proc.Id -ne $gpuPid){throw 'Unexpected DWM restart'}
   $mods=@($proc.Modules | Where-Object {$_.ModuleName -match 'bc250|vulkan_radeon'} | ForEach-Object {@{name=$_.ModuleName;path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
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
   $measuredSeconds=$watch.Elapsed.TotalSeconds
   @{utc=[DateTime]::UtcNow.ToString('o');elapsed=$measuredSeconds;pid=$proc.Id;cpu_seconds=$proc.CPU;temperature_c=$temperature;modules=$mods;log_bytes=(Get-Item $log).Length} | ConvertTo-Json -Depth 5 | Set-Content "$d\process-$i-$gpuPid.json"
   if($i -eq 0){& $cli log summary *> "$d\kmd-start.log"}
   if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
   if($i -in 2,8,14,20,26){
    & $cli fbdump "$d\dynamic-$i.bmp" *> "$d\dynamic-$i-dump.log"
    if($LASTEXITCODE -ne 0){throw 'Dynamic primary dump failed'}
    Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$d\dynamic-$i.png" -TimeoutSec 5
   }
   $i++
  }
  & $cli log summary *> "$d\kmd-end.log"
  & $cli fbdump "$d\gpu.bmp" *> "$d\gpu-dump.log"
  if($LASTEXITCODE -ne 0){throw 'GPU primary dump failed'}
  Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$d\screen.png" -TimeoutSec 5
  $success=$true

 } finally {& "$d\restore.ps1" -Restart}
} catch {$_ | Out-String;throw} finally {
 if($traceStarted){& logman stop BC250G0Dwm014 -ets *> "$d\etw-stop.log"}
 Get-ScheduledTask -TaskName $controlTask -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
 Get-Process composition-control -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq "$d\composition-control.exe"} | Stop-Process -Force -ErrorAction SilentlyContinue
 @{utc=[DateTime]::UtcNow.ToString('o');success=$success;gpu_pid=$gpuPid;measured_seconds=$measuredSeconds} | ConvertTo-Json | Set-Content "$d\done.json"
 Stop-Transcript | Out-Null
}
