$ErrorActionPreference='Stop'
$root='C:\BC250\m13\runtime-probe001'
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
if((Get-FileHash $umd).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected CPU UMD'}
$info=New-Object System.Diagnostics.ProcessStartInfo
$info.FileName="$root\runtime-control.exe"
$info.Arguments='baseline'
$info.WorkingDirectory=$root
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
$p=New-Object System.Diagnostics.Process
$p.StartInfo=$info
[void]$p.Start()
$o=$p.StandardOutput.ReadToEndAsync()
$e=$p.StandardError.ReadToEndAsync()
if(!$p.WaitForExit(45000)){Stop-Process -Id $p.Id -Force; $code=124}else{$code=$p.ExitCode}
$o.Result | Set-Content "$root\baseline-control001.stdout"
$e.Result | Set-Content "$root\baseline-control001.stderr"
"exit=$code"
$o.Result
$e.Result
Get-FileHash $umd,"$root\runtime-control.exe" | Select-Object Path,Hash | ConvertTo-Json
exit $code
