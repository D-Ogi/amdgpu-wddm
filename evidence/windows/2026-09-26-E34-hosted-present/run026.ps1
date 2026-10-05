$ErrorActionPreference='Stop'
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
$hosted='C:\BC250\m13\hosted-runtime014'
if((Get-FileHash "$hosted\vulkan_radeon.dll").Hash -ne '8341308E8AB6DAD13D02395915479E996F70A44C74CB2AFE646B557DAADB98BF'){throw 'Hosted ICD hash mismatch'}
if((Get-FileHash "$hosted\bc250d3d_zink.dll").Hash -ne 'D0BB42E7846E3C6738A0F56EE5E5327FF6F5A6E10282532595E7063722953738'){throw 'Hosted UMD hash mismatch'}
Copy-Item "$root\bc250d3d_zink.dll" "$hosted\previous-umd026.dll"
if((Get-FileHash "$hosted\runtime-present-control.exe").Hash -ne 'CF67B66985D29F25C0480322BE0E9C8BDA50B3A9DF1E666C3B196F766EE0C457'){throw 'Control hash mismatch'}
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$renamed=$false
$code=125
$p=$null
$appChanged=$false
try {
 $appChanged=$true
 Copy-Item "$hosted\bc250d3d_zink.dll" "$root\bc250d3d_zink.dll" -Force
 Move-Item -LiteralPath $umd -Destination $backup
 $renamed=$true
 Copy-Item -LiteralPath "$root\router.dll" -Destination $umd
 Copy-Item -LiteralPath $candidate -Destination $active -Force
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa026.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$hosted\runtime-present-control.exe"
 $info.WorkingDirectory=$root
 $info.UseShellExecute=$false
 $info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true
 $info.RedirectStandardError=$true
 $info.EnvironmentVariables['MESA_SHADER_CACHE_DIR']="$hosted\cache"
 $info.EnvironmentVariables['BC250_HOSTED_RENDER']='1'
 $info.EnvironmentVariables['BC250_HOSTED_ICD']='C:\BC250\m13\hosted-runtime014\vulkan_radeon.dll'
 $p=New-Object System.Diagnostics.Process
 $p.StartInfo=$info
 [void]$p.Start()
 $stdout=$p.StandardOutput.ReadToEndAsync()
 $stderr=$p.StandardError.ReadToEndAsync()
 $watch=[Diagnostics.Stopwatch]::StartNew()
 $captured=0
 while(!$p.WaitForExit(250)) {
  if($watch.Elapsed.TotalSeconds -ge 45){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}; break}
  if($captured -lt 3 -and $watch.Elapsed.TotalSeconds -ge (3+4*$captured)) {
   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$hosted\screen026-$captured.png" -TimeoutSec 3
   "capture=$captured elapsed_ms=$($watch.ElapsedMilliseconds)"
   ++$captured
  }
 }
 $code=if($watch.Elapsed.TotalSeconds -ge 45){124}else{$p.ExitCode}
 "captures=$captured"
 $stdout.Result | Set-Content "$root\stdout026.txt"
 $stderr.Result | Set-Content "$root\stderr026.txt"
 "exit=$code"
 Get-Content "$root\stdout026.txt"
 Get-Content "$root\stderr026.txt"
 if(Test-Path "$root\mesa026.log"){Get-Content "$root\mesa026.log"}
 Get-FileHash "$hosted\runtime-present-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
 } finally {
 $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
 try {
  if($p -and !$p.HasExited){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}}
 } catch {$restoreErrors.Add("process: $($_.Exception.Message)")}
 try {
  if($renamed){
   if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$hosted\router-used026.dll"}
   Move-Item -LiteralPath $backup -Destination $umd
  }
  if((Get-FileHash $umd).Hash -ne $originalUmd){throw 'CPU UMD restore mismatch'}
  'CPU UMD file restored'
 } catch {$restoreErrors.Add("CPU UMD: $($_.Exception.Message)")}
 try {
  Copy-Item -LiteralPath "$root\baseline.dll" -Destination $active -Force
  $restored=(Get-FileHash $active).Hash
  "restored=$restored"
  if($restored -ne $original){throw 'Baseline restore mismatch'}
 } catch {$restoreErrors.Add("system ICD: $($_.Exception.Message)")}
 try {
  if($appChanged){
   if(Test-Path "$root\bc250d3d_zink.dll"){Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$hosted\umd-held026.dll"}
   Copy-Item "$hosted\previous-umd026.dll" "$root\bc250d3d_zink.dll"
   if((Get-FileHash "$root\bc250d3d_zink.dll").Hash -ne (Get-FileHash "$hosted\previous-umd026.dll").Hash){throw 'App UMD restore mismatch'}
  }
 } catch {$restoreErrors.Add("app UMD: $($_.Exception.Message)")}
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 if($restoreErrors.Count){throw ($restoreErrors -join '; ')}
}
exit $code
