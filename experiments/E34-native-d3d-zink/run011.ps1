$ErrorActionPreference='Stop'
$root='C:\BC250\m13\native-zink001'
Set-Location -LiteralPath $root
$env:MESA_LOG_FILE="$root\mesa011.log"
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
$finished=$p.WaitForExit(45000)
if (!$finished) { Stop-Process -Id $p.Id -Force; 'TIMEOUT'; exit 124 }
$stdout.Result | Set-Content "$root\stdout011.txt"
$stderr.Result | Set-Content "$root\stderr011.txt"
"exit=$($p.ExitCode)"
Get-Content "$root\stdout011.txt"
Get-Content "$root\stderr011.txt"
if(Test-Path "$root\mesa011.log"){Get-Content "$root\mesa011.log"}
Get-FileHash "$root\native-control.exe","$root\bc250d3d_zink.dll" | Select-Object Path,Hash | ConvertTo-Json
exit $p.ExitCode
