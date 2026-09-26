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
$dwmBefore=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$renamed=$false
$code=125
try {
 Move-Item -LiteralPath $umd -Destination $backup
 $renamed=$true
 Copy-Item -LiteralPath "$root\router.dll" -Destination $umd
 Copy-Item -LiteralPath $candidate -Destination $active -Force
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa003.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$root\runtime-control.exe"
 $info.WorkingDirectory=$root
 $info.UseShellExecute=$false
 $info.CreateNoWindow=$true
 $info.RedirectStandardOutput=$true
 $info.RedirectStandardError=$true
 $p=New-Object System.Diagnostics.Process
 $p.StartInfo=$info
 [void]$p.Start()
 $stdout=$p.StandardOutput.ReadToEndAsync()
 $stderr=$p.StandardError.ReadToEndAsync()
 if(!$p.WaitForExit(45000)){Stop-Process -Id $p.Id -Force; $code=124}else{$code=$p.ExitCode}
 $stdout.Result | Set-Content "$root\stdout003.txt"
 $stderr.Result | Set-Content "$root\stderr003.txt"
 "exit=$code"
 Get-Content "$root\stdout003.txt"
 Get-Content "$root\stderr003.txt"
 if(Test-Path "$root\mesa003.log"){Get-Content "$root\mesa003.log"}
 Get-FileHash "$root\runtime-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
} finally {
 if($renamed){
  if(Test-Path $umd){Move-Item -LiteralPath $umd -Destination "$root\router-used003.dll"}
  Move-Item -LiteralPath $backup -Destination $umd
 }
 if((Get-FileHash $umd).Hash -ne $originalUmd){throw 'CPU UMD restore mismatch'}
 'CPU UMD file restored'
 "DWM before=$($dwmBefore -join ',') after=$((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')"
 Copy-Item -LiteralPath "$root\baseline.dll" -Destination $active -Force
 $restored=(Get-FileHash $active).Hash
 "restored=$restored"
 if($restored -ne $original){throw 'Baseline restore mismatch'}
}
exit $code
