$ErrorActionPreference='Stop'
# E36 fork-branch control 2: run031 (M546 flip-model regression) with the consolidated-branch ICD and UMD.
# Differences from run031: candidate system ICD, hosted ICD and hosted UMD come from fork-consolidated001;
# the control executable and the router are the recorded ones; outputs carry the fork001 suffix.
$root='C:\BC250\m13\runtime-probe001'
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$original='9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$fork='C:\BC250\m13\fork-consolidated001'
$candidate="$fork\vulkan_radeon.dll"
$candidateHash='FAD08ECB16C9CFFDCB8D09D408C8ABC428B348D948D945D78DE30892D88A65AF'
$umdHash='848BCBF5FD3B864B183F2194308FA70D254C615B5B30BDBC602C34C9EDC00287'
$recorded='C:\BC250\m13\hosted-runtime016'
if((Get-FileHash $active).Hash -ne $original){throw 'Unexpected baseline'}
if((Get-FileHash $candidate).Hash -ne $candidateHash){throw 'Unexpected candidate'}
if((Get-FileHash "$fork\bc250d3d_zink.dll").Hash -ne $umdHash){throw 'Unexpected fork UMD'}
Copy-Item -LiteralPath $active -Destination "$root\baseline.dll"
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
$backup='C:\BC250\m11\resource-close\bc250d3d.m13-original.dll'
$originalUmd=(Get-FileHash $umd).Hash
if($originalUmd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected CPU UMD'}
if(Test-Path $backup){throw 'Unresolved prior UMD backup'}
if(-not (Test-Path "$root\router.dll")){throw 'Recorded router missing'}
if((Get-FileHash "$recorded\runtime-flip-control.exe").Hash -ne 'E58D29517DB7C5E29B30F73E19F9EAB4C6C376AC411E2E28E6871E74224642BA'){throw 'Control hash mismatch'}
Copy-Item "$root\bc250d3d_zink.dll" "$fork\previous-umd-fork001.dll"
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
 $env:MESA_LOG_FILE="$fork\mesa-fork001.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$recorded\runtime-flip-control.exe"
 $info.WorkingDirectory=$root
 $info.UseShellExecute=$false
 $info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true
 $info.RedirectStandardError=$true
 $info.EnvironmentVariables['MESA_SHADER_CACHE_DIR']="$fork\cache"
 $info.EnvironmentVariables['BC250_HOSTED_RENDER']='1'
 $info.EnvironmentVariables['BC250_HOSTED_ICD']="$fork\vulkan_radeon.dll"
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
   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$fork\screen-fork001-$captured.png" -TimeoutSec 3
   "capture=$captured elapsed_ms=$($watch.ElapsedMilliseconds)"
   ++$captured
  }
 }
 $code=if($watch.Elapsed.TotalSeconds -ge 45){124}else{$p.ExitCode}
 "captures=$captured"
 $stdout.Result | Set-Content "$fork\stdout-fork001.txt"
 $stderr.Result | Set-Content "$fork\stderr-fork001.txt"
 "exit=$code"
 Get-Content "$fork\stdout-fork001.txt"
 Get-Content "$fork\stderr-fork001.txt"
 if(Test-Path "$fork\mesa-fork001.log"){Get-Content "$fork\mesa-fork001.log"}
 Get-FileHash "$recorded\runtime-flip-control.exe","$root\bc250d3d_zink.dll",$active,"$fork\vulkan_radeon.dll" | Select-Object Path,Hash | ConvertTo-Json
 } finally {
 $restoreErrors=New-Object 'System.Collections.Generic.List[string]'
 try {
  if($p -and !$p.HasExited){$p.Kill(); if(!$p.WaitForExit(5000)){throw 'Control remains live'}}
 } catch {$restoreErrors.Add("process: $($_.Exception.Message)")}
 try {
  if($renamed){
   if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$fork\router-used-fork001.dll"}
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
   if(Test-Path "$root\bc250d3d_zink.dll"){Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$fork\umd-held-fork001.dll"}
   Copy-Item "$fork\previous-umd-fork001.dll" "$root\bc250d3d_zink.dll"
   if((Get-FileHash "$root\bc250d3d_zink.dll").Hash -ne (Get-FileHash "$fork\previous-umd-fork001.dll").Hash){throw 'App UMD restore mismatch'}
  }
 } catch {$restoreErrors.Add("app UMD: $($_.Exception.Message)")}
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 if($restoreErrors.Count){throw ($restoreErrors -join '; ')}
}
exit $code
