$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
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
$hosted='C:\BC250\m13\hosted-runtime039'
if((Get-FileHash "$hosted\vulkan_radeon.dll").Hash -ne '3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71'){throw 'Hosted ICD hash mismatch'}
if((Get-FileHash "$hosted\bc250d3d_zink.dll").Hash -ne '50491FA3ED246ED33159FB4DBE0C830B27C96E676F3AE5E9DAF7A5E3EEA39DB3'){throw 'Hosted UMD hash mismatch'}
Copy-Item "$root\bc250d3d_zink.dll" "$hosted\previous-umd091.dll"
if((Get-FileHash "$hosted\graphics-batch-control.exe").Hash -ne '732DD8DC3866580F47125B52AF82128F0CE8467D8D2570174C14778D4234B43C'){throw 'Control hash mismatch'}
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
if(Test-Path "$root\stdout091.txt"){throw 'Existing run'}
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
 Move-Item -LiteralPath $active -Destination "$hosted\system-original091.dll"
 $icdRenamed=$true
 Copy-Item -LiteralPath $candidate -Destination $active
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa091.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$hosted\graphics-batch-control.exe"
 $info.Arguments='hosted batch'
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
 $info.EnvironmentVariables['BC250_HOSTED_ICD']='C:\BC250\m13\hosted-runtime039\vulkan_radeon.dll'
 $p=New-Object System.Diagnostics.Process
 $p.StartInfo=$info
 & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$hosted\kmd-before091.log"
 if($LASTEXITCODE -ne 0){throw 'KMD baseline counters failed'}
 [void]$p.Start()
 $controlStarted=$true
 $stdout=$p.StandardOutput.ReadToEndAsync()
 $stderr=$p.StandardError.ReadToEndAsync()
 $watch=[Diagnostics.Stopwatch]::StartNew()
 $captured=0
 while(!$p.WaitForExit(250)) {
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){$p.Kill();[void]$p.WaitForExit(5000);throw 'Owner STOP'}
  if($watch.Elapsed.TotalSeconds -ge 180){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}; break}

 }
 $code=if($watch.Elapsed.TotalSeconds -ge 180){124}else{$p.ExitCode}
 Start-Sleep -Seconds 5
 & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$hosted\kmd-after091.log"
 if($LASTEXITCODE -ne 0){throw 'KMD final counters failed'}
 "captures=$captured"
 $stdout.Result | Set-Content "$root\stdout091.txt"
 $stderr.Result | Set-Content "$root\stderr091.txt"
 "exit=$code"
 Get-Content "$root\stdout091.txt"
 Get-Content "$root\stderr091.txt"
 if(Test-Path "$root\mesa091.log"){Get-Content "$root\mesa091.log"}
 Get-FileHash "$hosted\graphics-batch-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
 } finally {
 $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
 try {
  if($controlStarted -and !$p.HasExited){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}}
 } catch {$restoreErrors.Add("process: $($_.Exception.Message)")}
 try {
  if($renamed){
   if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$hosted\router-used091.dll"}
   Move-Item -LiteralPath $backup -Destination $umd
  }
  if((Get-FileHash $umd).Hash -ne $originalUmd){throw 'CPU UMD restore mismatch'}
  'CPU UMD file restored'
 } catch {$restoreErrors.Add("CPU UMD: $($_.Exception.Message)")}
 try {
  if($icdRenamed){
   if(Test-Path $active){Move-Item -LiteralPath $active -Destination "$hosted\system-candidate-held091.dll"}
   Move-Item -LiteralPath "$hosted\system-original091.dll" -Destination $active
  }
  $restored=(Get-FileHash $active).Hash
  "restored=$restored"
  if($restored -ne $original){throw 'Baseline restore mismatch'}
 } catch {$restoreErrors.Add("system ICD: $($_.Exception.Message)")}
 try {
  if($appChanged){
   if(Test-Path "$root\bc250d3d_zink.dll"){Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$hosted\umd-held091.dll"}
   Copy-Item "$hosted\previous-umd091.dll" "$root\bc250d3d_zink.dll"
   if((Get-FileHash "$root\bc250d3d_zink.dll").Hash -ne (Get-FileHash "$hosted\previous-umd091.dll").Hash){throw 'App UMD restore mismatch'}
  }
 } catch {$restoreErrors.Add("app UMD: $($_.Exception.Message)")}
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 if($restoreErrors.Count){throw ($restoreErrors -join '; ')}
 $dwmAfter=@(Get-Process dwm | Select-Object -ExpandProperty Id)
 if(Compare-Object $dwmBefore $dwmAfter){throw 'Unexpected DWM change'}
 @{exit=$code;dwm_before=$dwmBefore;dwm_after=$dwmAfter;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$hosted\done091.json"
}
exit $code
