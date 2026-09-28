$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$health=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read | Out-String
if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A0 flags=15'){throw 'KMD160 health gate'}
$clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
if(@(Get-Process witcher3,deqp-vk,vkcube,gfx-blt-control,cross-process-control,cross-process-duplicate-control -ErrorAction SilentlyContinue).Count){throw 'Another test is active'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
if((Get-FileHash -LiteralPath $image).Hash -ne '8E676C810190EF38BACF3E8332EDC2760E07844FD779E18CB32B22D24AC90218'){throw 'Unexpected KMD SYS'}
$root='C:\BC250\m13\runtime-probe001' 
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$original='93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$candidate='C:\BC250\m13\shared-import001\vulkan_radeon.dll'
if((Get-FileHash $active).Hash -ne $original){throw 'Unexpected baseline'}
if((Get-FileHash $candidate).Hash -ne '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'){throw 'Unexpected candidate'}
Copy-Item -LiteralPath $active -Destination "$root\baseline.dll"
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
$backup='C:\BC250\m11\resource-close\bc250d3d.m13-original.dll'
$originalUmd=(Get-FileHash $umd).Hash
if($originalUmd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected CPU UMD'}
if(Test-Path $backup){throw 'Unresolved prior UMD backup'}
$hosted='C:\BC250\m13\hosted-runtime053'
if((Get-FileHash "$hosted\vulkan_radeon.dll").Hash -ne '3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71'){throw 'Hosted ICD hash mismatch'}
if((Get-FileHash "$hosted\bc250d3d_zink.dll").Hash -ne '5C74BF98F2362127173C53F7808950D4B6676E1B1C901A341163F7032AE78965'){throw 'Hosted UMD hash mismatch'}
Copy-Item "$root\bc250d3d_zink.dll" "$hosted\previous-umd127.dll"
if((Get-FileHash "$hosted\cross-process-duplicate-control.exe").Hash -ne '54D241077BA7A6609F9FB60EB2D62B9ECD917905042CC764351661EDF460EB9A'){throw 'Control hash mismatch'}
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
if(Test-Path "$root\stdout127.txt"){throw 'Existing run'}
if((Get-FileHash "$root\router.dll").Hash -ne 'B2748CF01D6490AC225FBC6CA349F84C4DA0BE103EABFBEA97B8EB90C95079ED'){throw 'Router hash mismatch'}
$renamed=$false
$icdRenamed=$false
$code=125
$p=$null
$controlStarted=$false
$appChanged=$false
try {
 $appChanged=$true
 Copy-Item "$hosted\bc250d3d_zink.dll" "$root\bc250d3d_zink.dll" -Force
 Move-Item -LiteralPath $umd -Destination $backup
 $renamed=$true
 Copy-Item -LiteralPath "$root\router.dll" -Destination $umd
 Move-Item -LiteralPath $active -Destination "$hosted\system-original127.dll"
 $icdRenamed=$true
 Copy-Item -LiteralPath $candidate -Destination $active
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa127.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$hosted\cross-process-duplicate-control.exe"
 $info.Arguments='hosted 101 normal duplicate'
 $info.WorkingDirectory=$root
 $info.UseShellExecute=$false
 $info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true
 $info.RedirectStandardError=$true
 $info.EnvironmentVariables['MESA_SHADER_CACHE_DIR']="$hosted\cache"
 $info.EnvironmentVariables['BC250_HOSTED_RENDER']='1'
 $info.EnvironmentVariables['BC250_HOST_AUDIT']='1'
 $info.EnvironmentVariables.Remove('BC250_HOST_TRACE_SAMPLER')
 $info.EnvironmentVariables['MESA_SHADER_CACHE_DISABLE']='true'
 $info.EnvironmentVariables['BC250_HOSTED_ICD']='C:\BC250\m13\hosted-runtime053\vulkan_radeon.dll'
 $p=New-Object System.Diagnostics.Process
 $p.StartInfo=$info
 & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$hosted\kmd-before127.log"
 if($LASTEXITCODE -ne 0){throw 'KMD baseline counters failed'}
 [void]$p.Start()
 $controlStarted=$true
 $stdout=$p.StandardOutput.ReadToEndAsync()
 $stderr=$p.StandardError.ReadToEndAsync()
 $watch=[Diagnostics.Stopwatch]::StartNew()
 $captured=0
 while(!$p.WaitForExit(250)) {
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){$p.Kill();[void]$p.WaitForExit(5000);throw 'Owner STOP'}
  $thermal=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
  if($LASTEXITCODE -ne 0 -or $thermal -notmatch 'temperature_mc=(\d+)' -or [int]$Matches[1] -ge 85000){$p.Kill();[void]$p.WaitForExit(5000);throw 'Thermal/telemetry stop'}
  if($watch.Elapsed.TotalSeconds -ge 180){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}; break}

 }
 $code=if($watch.Elapsed.TotalSeconds -ge 180){124}else{$p.ExitCode}
 Start-Sleep -Seconds 5
 & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$hosted\kmd-after127.log"
 if($LASTEXITCODE -ne 0){throw 'KMD final counters failed'}
 "captures=$captured"
 $stdout.Result | Set-Content "$root\stdout127.txt"
 if($code -eq 0 -and ($stdout.Result -notmatch 'duplicate_imports PASS reopens=2 independent_devices=3' -or $stdout.Result -notmatch 'PASS iterations=101 generations=2' -or $stdout.Result -notmatch 'owner_exit=normal child_exit=0' -or $stdout.Result -match 'FAIL')){$code=1}
 $stderr.Result | Set-Content "$root\stderr127.txt"
 "exit=$code"
 Get-Content "$root\stdout127.txt"
 Get-Content "$root\stderr127.txt"
 if(Test-Path "$root\mesa127.log"){Get-Content "$root\mesa127.log"}
 Get-FileHash "$hosted\cross-process-duplicate-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
 } finally {
 $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
 try {
  if($controlStarted -and !$p.HasExited){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}}
 } catch {$restoreErrors.Add("process: $($_.Exception.Message)")}
 try {
  if($renamed){
   if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$hosted\router-used127.dll"}
   Move-Item -LiteralPath $backup -Destination $umd
  }
  if((Get-FileHash $umd).Hash -ne $originalUmd){throw 'CPU UMD restore mismatch'}
  'CPU UMD file restored'
 } catch {$restoreErrors.Add("CPU UMD: $($_.Exception.Message)")}
 try {
  if($icdRenamed){
   if(Test-Path $active){Move-Item -LiteralPath $active -Destination "$hosted\system-candidate-held127.dll"}
   Move-Item -LiteralPath "$hosted\system-original127.dll" -Destination $active
  }
  $restored=(Get-FileHash $active).Hash
  "restored=$restored"
  if($restored -ne $original){throw 'Baseline restore mismatch'}
 } catch {$restoreErrors.Add("system ICD: $($_.Exception.Message)")}
 try {
  if($appChanged){
   if(Test-Path "$root\bc250d3d_zink.dll"){Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$hosted\umd-held127.dll"}
   Copy-Item "$hosted\previous-umd127.dll" "$root\bc250d3d_zink.dll"
   if((Get-FileHash "$root\bc250d3d_zink.dll").Hash -ne (Get-FileHash "$hosted\previous-umd127.dll").Hash){throw 'App UMD restore mismatch'}
  }
 } catch {$restoreErrors.Add("app UMD: $($_.Exception.Message)")}
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 if($restoreErrors.Count){throw ($restoreErrors -join '; ')}
 $dwmAfter=@(Get-Process dwm | Select-Object -ExpandProperty Id)
 if(Compare-Object $dwmBefore $dwmAfter){throw 'Unexpected DWM change'}
 @{exit=$code;dwm_before=$dwmBefore;dwm_after=$dwmAfter;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$hosted\done127.json"
}
exit $code
