$ErrorActionPreference='Stop'
$root='C:\BC250\m13\shared-import001'
Set-Location -LiteralPath $root
$env:MESA_LOG_FILE="$root\mesa006.log"
$info=New-Object System.Diagnostics.ProcessStartInfo
$info.FileName="$root\shared-import.exe"
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
$stdout.Result | Set-Content "$root\stdout006.txt"
$stderr.Result | Set-Content "$root\stderr006.txt"
"exit=$($p.ExitCode)"
Get-Content "$root\stdout006.txt"
Get-Content "$root\stderr006.txt"
if(Test-Path "$root\mesa006.log"){Get-Content "$root\mesa006.log"}
Get-FileHash "$root\shared-import.exe","$root\vulkan_radeon.dll" | Select-Object Path,Hash | ConvertTo-Json
exit $p.ExitCode
