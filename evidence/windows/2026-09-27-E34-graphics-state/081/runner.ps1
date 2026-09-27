$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\graphics-state081'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if(Test-Path "$dir\done.json"){throw 'Existing run'}
if((Get-FileHash "$dir\graphics-state-control.exe").Hash -ne '0C1065412592E4B42176874C4AC43280CDCF2114D1EF3365174FBA0F4A982073'){throw 'Control hash mismatch'}
if((Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Baseline UMD mismatch'}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'Baseline ICD mismatch'}
$before=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName="$dir\graphics-state-control.exe"
$info.Arguments='baseline'
$info.WorkingDirectory=$dir
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
$info.EnvironmentVariables.Remove('BC250_D3D_RUNTIME_PROBE')
$info.EnvironmentVariables['MESA_LOG_FILE']="$dir\mesa.log"
$process=New-Object Diagnostics.Process
$process.StartInfo=$info
$code=125
try {
 [void]$process.Start()
 $stdout=$process.StandardOutput.ReadToEndAsync()
 $stderr=$process.StandardError.ReadToEndAsync()
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while(!$process.WaitForExit(500)) {
  if($watch.Elapsed.TotalSeconds -ge 90 -or (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){
   $process.Kill()
   if(!$process.WaitForExit(5000)){throw 'Parent remains live'}
   $code=124
   break
  }
 }
 if($code -ne 124){$code=$process.ExitCode}
 $stdout.Result | Set-Content "$dir\stdout.txt"
 $stderr.Result | Set-Content "$dir\stderr.txt"
 Get-Content "$dir\stdout.txt"
 Get-Content "$dir\stderr.txt"
} finally {
 if($process.Id -and !$process.HasExited){$process.Kill();[void]$process.WaitForExit(5000)}
 $after=@(Get-Process dwm | Select-Object -ExpandProperty Id)
 @{exit=$code;dwm_before=$before;dwm_after=$after;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$dir\done.json"
}
exit $code
