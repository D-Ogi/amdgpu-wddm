$ErrorActionPreference='Stop'
$root='C:\BC250\m13\native-shared001'
$active='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$original='9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$candidate='C:\BC250\m13\shared-import001\vulkan_radeon.dll'
if((Get-FileHash $active).Hash -ne $original){throw 'Unexpected baseline'}
if((Get-FileHash $candidate).Hash -ne '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'){throw 'Unexpected candidate'}
Copy-Item -LiteralPath $active -Destination "$root\baseline.dll"
$code=125
try {
 Copy-Item -LiteralPath $candidate -Destination $active -Force
 Set-Location -LiteralPath $root
 $env:MESA_LOG_FILE="$root\mesa002.log"
 $info=New-Object System.Diagnostics.ProcessStartInfo
 $info.FileName="$root\native-control.exe"
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
 $stdout.Result | Set-Content "$root\stdout002.txt"
 $stderr.Result | Set-Content "$root\stderr002.txt"
 "exit=$code"
 Get-Content "$root\stdout002.txt"
 Get-Content "$root\stderr002.txt"
 if(Test-Path "$root\mesa002.log"){Get-Content "$root\mesa002.log"}
 Get-FileHash "$root\native-control.exe","$root\bc250d3d_zink.dll",$active | Select-Object Path,Hash | ConvertTo-Json
} finally {
 Copy-Item -LiteralPath "$root\baseline.dll" -Destination $active -Force
 $restored=(Get-FileHash $active).Hash
 "restored=$restored"
 if($restored -ne $original){throw 'Baseline restore mismatch'}
}
exit $code
