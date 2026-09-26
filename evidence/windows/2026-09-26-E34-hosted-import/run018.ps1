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
$hosted='C:\BC250\m13\hosted-runtime010'
if((Get-FileHash "$hosted\vulkan_radeon.dll").Hash -ne 'C798E45E93CA25E50DDE1FCC183E091E4BFD43D2DCDC0D6DC3CAA646A4EAACDE'){throw 'Hosted ICD hash mismatch'}
if((Get-FileHash "$hosted\bc250d3d_zink.dll").Hash -ne '86C655307C2666151BB3FAD6BA09B1A67490FC751C395722D7781021B5AF799C'){throw 'Hosted UMD hash mismatch'}
Copy-Item "$root\bc250d3d_zink.dll" "$hosted\previous-umd.dll"
Copy-Item "$hosted\bc250d3d_zink.dll" "$root\bc250d3d_zink.dll" -Force
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$renamed=$false
$code=125
try {
 Move-Item -LiteralPath $umd -Destination $backup
 $renamed=$true
 Copy-Item -LiteralPath "$root\router.dll" -Destination $umd
 Copy-Item -LiteralPath $candidate -Destination $active -Force
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa018.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$hosted\runtime-shared-control.exe"
 $info.WorkingDirectory=$root
 $info.UseShellExecute=$false
 $info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true
 $info.RedirectStandardError=$true
 $info.EnvironmentVariables['MESA_SHADER_CACHE_DIR']="$hosted\cache"
 $info.EnvironmentVariables['BC250_HOSTED_RENDER']='1'
 $info.EnvironmentVariables['BC250_HOSTED_ICD']='C:\BC250\m13\hosted-runtime010\vulkan_radeon.dll'
 $p=New-Object System.Diagnostics.Process
 $p.StartInfo=$info
 [void]$p.Start()
 $stdout=$p.StandardOutput.ReadToEndAsync()
 $stderr=$p.StandardError.ReadToEndAsync()
 if(!$p.WaitForExit(45000)){Stop-Process -Id $p.Id -Force; $code=124}else{$code=$p.ExitCode}
 $stdout.Result | Set-Content "$root\stdout018.txt"
 $stderr.Result | Set-Content "$root\stderr018.txt"
 "exit=$code"
 Get-Content "$root\stdout018.txt"
 Get-Content "$root\stderr018.txt"
 if(Test-Path "$root\mesa018.log"){Get-Content "$root\mesa018.log"}
 Get-FileHash "$root\runtime-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
} finally {
 Move-Item -LiteralPath "$root\bc250d3d_zink.dll" -Destination "$hosted\umd-held.dll"
 Copy-Item "$hosted\previous-umd.dll" "$root\bc250d3d_zink.dll"
 if($renamed){
  if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$root\router-used018.dll"}
  Move-Item -LiteralPath $backup -Destination $umd
 }
 if((Get-FileHash $umd).Hash -ne $originalUmd){throw 'CPU UMD restore mismatch'}
 'CPU UMD file restored'
 Get-FileHash "$hosted\vulkan_radeon.dll","$hosted\bc250d3d_zink.dll" | Select-Object Path,Hash | ConvertTo-Json
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 Copy-Item -LiteralPath "$root\baseline.dll" -Destination $active -Force
 $restored=(Get-FileHash $active).Hash
 "restored=$restored"
 if($restored -ne $original){throw 'Baseline restore mismatch'}
}
exit $code
