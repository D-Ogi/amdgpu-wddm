$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$root='C:\BC250\m13\runtime-probe001'
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$original='9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$candidate='C:\BC250\m13\shared-import001\vulkan_radeon.dll'
if((Get-FileHash $active).Hash -ne $original){throw 'Unexpected baseline'}
if((Get-FileHash $candidate).Hash -ne '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'){throw 'Unexpected candidate'}
Copy-Item -LiteralPath $active -Destination "$root\baseline.dll"
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
$backup='C:\BC250\m11\resource-close\bc250d3d.m13-original.dll'
$originalUmd=(Get-FileHash $umd).Hash
if($originalUmd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected CPU UMD'}
if(Test-Path $backup){throw 'Unresolved prior UMD backup'}
$hosted='C:\BC250\m13\hosted-runtime033'
if((Get-FileHash "$hosted\vulkan_radeon.dll").Hash -ne '3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71'){throw 'Hosted ICD hash mismatch'}
if((Get-FileHash "$hosted\bc250d3d_zink.dll").Hash -ne 'F8BB0D3EDD6A777AB6B7EC2633FE1D512B5808D311924325E7AADFB6E415630C'){throw 'Hosted UMD hash mismatch'}
Copy-Item "$root\bc250d3d_zink.dll" "$hosted\previous-umd073.dll"
if((Get-FileHash "$hosted\cross-process-control.exe").Hash -ne '64B47285C360C91D1E6899C47219175BED869F0774F888A856A44100C6A03059'){throw 'Control hash mismatch'}
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
if(Test-Path "$root\stdout073.txt"){throw 'Existing run'}
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
 Move-Item -LiteralPath $active -Destination "$hosted\system-original073.dll"
 $icdRenamed=$true
 Copy-Item -LiteralPath $candidate -Destination $active
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa073.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$hosted\cross-process-control.exe"
 $info.Arguments='hosted 1000'
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
 $info.EnvironmentVariables['BC250_HOSTED_ICD']='C:\BC250\m13\hosted-runtime033\vulkan_radeon.dll'
 $p=New-Object System.Diagnostics.Process
 $p.StartInfo=$info
 & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$hosted\kmd-before073.log"
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
 & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$hosted\kmd-after073.log"
 if($LASTEXITCODE -ne 0){throw 'KMD final counters failed'}
 "captures=$captured"
 $stdout.Result | Set-Content "$root\stdout073.txt"
 $stderr.Result | Set-Content "$root\stderr073.txt"
 "exit=$code"
 Get-Content "$root\stdout073.txt"
 Get-Content "$root\stderr073.txt"
 if(Test-Path "$root\mesa073.log"){Get-Content "$root\mesa073.log"}
 Get-FileHash "$hosted\cross-process-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
 } finally {
 $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
 try {
  if($controlStarted -and !$p.HasExited){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}}
 } catch {$restoreErrors.Add("process: $($_.Exception.Message)")}
 try {
  if($renamed){
   if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$hosted\router-used073.dll"}
   Move-Item -LiteralPath $backup -Destination $umd
  }
  if((Get-FileHash $umd).Hash -ne $originalUmd){throw 'CPU UMD restore mismatch'}
  'CPU UMD file restored'
 } catch {$restoreErrors.Add("CPU UMD: $($_.Exception.Message)")}
 try {
  if($icdRenamed){
   if(Test-Path $active){Move-Item -LiteralPath $active -Destination "$hosted\system-candidate-held073.dll"}
   Move-Item -LiteralPath "$hosted\system-original073.dll" -Destination $active
  }
  $restored=(Get-FileHash $active).Hash
  "restored=$restored"
  if($restored -ne $original){throw 'Baseline restore mismatch'}
 } catch {$restoreErrors.Add("system ICD: $($_.Exception.Message)")}
 try {
  if($appChanged){
   if(Test-Path "$root\bc250d3d_zink.dll"){Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$hosted\umd-held073.dll"}
   Copy-Item "$hosted\previous-umd073.dll" "$root\bc250d3d_zink.dll"
   if((Get-FileHash "$root\bc250d3d_zink.dll").Hash -ne (Get-FileHash "$hosted\previous-umd073.dll").Hash){throw 'App UMD restore mismatch'}
  }
 } catch {$restoreErrors.Add("app UMD: $($_.Exception.Message)")}
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 if($restoreErrors.Count){throw ($restoreErrors -join '; ')}
 $dwmAfter=@(Get-Process dwm | Select-Object -ExpandProperty Id)
 if(Compare-Object $dwmBefore $dwmAfter){throw 'Unexpected DWM change'}
 @{exit=$code;dwm_before=$dwmBefore;dwm_after=$dwmAfter;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$hosted\done073.json"
}
exit $code
