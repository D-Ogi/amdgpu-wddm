$ErrorActionPreference='Stop'
$d='C:\BC250\m13\hosted-runtime029'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$umd='C:\BC250\m11\resource-close\bc250d3d.dll'
if((Get-FileHash $umd).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected UMD'}
$before=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$info=New-Object System.Diagnostics.ProcessStartInfo
$info.FileName="$d\runtime-vertex-id-control.exe"
$info.Arguments='--warp'
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
$p=New-Object System.Diagnostics.Process
$p.StartInfo=$info
[void]$p.Start()
$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
if(!$p.WaitForExit(30000)){$p.Kill();$p.WaitForExit();throw 'Control timeout'}
$stdout.Result
$stderr.Result
$after=@(Get-Process dwm | Select-Object -ExpandProperty Id)
"DWM before=$before after=$after"
Get-FileHash $umd,"$d\runtime-vertex-id-control.exe" | Select-Object Path,Hash | ConvertTo-Json
exit $p.ExitCode
