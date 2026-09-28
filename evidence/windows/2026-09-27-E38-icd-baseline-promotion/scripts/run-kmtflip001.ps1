$ErrorActionPreference='Stop'
# M546 flip-model regression (run031 / E36 run-fork001 procedure) with the KMT-enumeration candidate ICD 93B1D1FD
# as system ICD and hosted ICD, and the fork-consolidated001 UMD 848BCBF5 in the router's application slot.
# Differences from run-fork001: candidate ICD path and hash, output directory C:\BC250\m13\kmt-flip001,
# outputs carry the kmtflip001 suffix. Control executable and router unchanged (recorded E58D2951).
$root='C:\BC250\m13\runtime-probe001'
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$original='9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$fork='C:\BC250\m13\fork-consolidated001'
$out='C:\BC250\m13\kmt-flip001'
$candidate='C:\BC250\m12\fl-probe001\candidate-kmt-enum\vulkan_radeon.dll'
$candidateHash='93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$umdHash='848BCBF5FD3B864B183F2194308FA70D254C615B5B30BDBC602C34C9EDC00287'
$recorded='C:\BC250\m13\hosted-runtime016'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if((Get-FileHash $active).Hash -ne $original){throw 'Unexpected baseline'}
if((Get-FileHash $candidate).Hash -ne $candidateHash){throw 'Unexpected candidate'}
if((Get-FileHash "$fork\bc250d3d_zink.dll").Hash -ne $umdHash){throw 'Unexpected fork UMD'}
if((Test-Path "$root\baseline.dll") -and ((Get-FileHash "$root\baseline.dll").Hash -ne $original)){throw 'Stale foreign baseline.dll in runtime-probe001'}
Copy-Item -LiteralPath $active -Destination "$root\baseline.dll" -Force
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
$backup='C:\BC250\m11\resource-close\bc250d3d.m13-original.dll'
$originalUmd=(Get-FileHash $umd).Hash
if($originalUmd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected CPU UMD'}
if(Test-Path $backup){throw 'Unresolved prior UMD backup'}
if(-not (Test-Path "$root\router.dll")){throw 'Recorded router missing'}
if((Get-FileHash "$recorded\runtime-flip-control.exe").Hash -ne 'E58D29517DB7C5E29B30F73E19F9EAB4C6C376AC411E2E28E6871E74224642BA'){throw 'Control hash mismatch'}
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "temperature_before=$($Matches[1])"; if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' } }
Copy-Item "$root\bc250d3d_zink.dll" "$out\previous-umd-kmtflip001.dll"
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$renamed=$false
$code=125
$p=$null
$appChanged=$false
try {
 $appChanged=$true
 Copy-Item "$fork\bc250d3d_zink.dll" "$root\bc250d3d_zink.dll" -Force
 Move-Item -LiteralPath $umd -Destination $backup
 $renamed=$true
 Copy-Item -LiteralPath "$root\router.dll" -Destination $umd
 Copy-Item -LiteralPath $candidate -Destination $active -Force
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$out\mesa-kmtflip001.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$recorded\runtime-flip-control.exe"
 $info.WorkingDirectory=$root
 $info.UseShellExecute=$false
 $info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true
 $info.RedirectStandardError=$true
 $info.EnvironmentVariables['MESA_SHADER_CACHE_DIR']="$out\cache"
 $info.EnvironmentVariables['BC250_HOSTED_RENDER']='1'
 $info.EnvironmentVariables['BC250_HOSTED_ICD']=$candidate
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
   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$out\screen-kmtflip001-$captured.png" -TimeoutSec 3
   "capture=$captured elapsed_ms=$($watch.ElapsedMilliseconds)"
   ++$captured
  }
 }
 $code=if($watch.Elapsed.TotalSeconds -ge 45){124}else{$p.ExitCode}
 "captures=$captured"
 $stdout.Result | Set-Content "$out\stdout-kmtflip001.txt"
 $stderr.Result | Set-Content "$out\stderr-kmtflip001.txt"
 "exit=$code"
 Get-Content "$out\stdout-kmtflip001.txt"
 Get-Content "$out\stderr-kmtflip001.txt"
 if(Test-Path "$out\mesa-kmtflip001.log"){Get-Content "$out\mesa-kmtflip001.log"}
 Get-FileHash "$recorded\runtime-flip-control.exe","$root\bc250d3d_zink.dll",$active,$candidate | Select-Object Path,Hash | ConvertTo-Json
 } finally {
 $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
 try {
  if($p -and !$p.HasExited){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}}
 } catch {$restoreErrors.Add("process: $($_.Exception.Message)")}
 try {
  if($renamed){
   if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$out\router-used-kmtflip001.dll"}
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
  Remove-Item -LiteralPath "$root\baseline.dll" -Force
 } catch {$restoreErrors.Add("system ICD: $($_.Exception.Message)")}
 try {
  if($appChanged){
   if(Test-Path "$root\bc250d3d_zink.dll"){Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$out\umd-held-kmtflip001.dll"}
   Copy-Item "$out\previous-umd-kmtflip001.dll" "$root\bc250d3d_zink.dll"
   if((Get-FileHash "$root\bc250d3d_zink.dll").Hash -ne (Get-FileHash "$out\previous-umd-kmtflip001.dll").Hash){throw 'App UMD restore mismatch'}
  }
 } catch {$restoreErrors.Add("app UMD: $($_.Exception.Message)")}
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 if($restoreErrors.Count){throw ($restoreErrors -join '; ')}
}
exit $code
