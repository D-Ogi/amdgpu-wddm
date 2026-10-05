param([string]$IcdHash)
$ErrorActionPreference='Stop'
$d=$PSScriptRoot
if((Get-FileHash (Join-Path $d 'amdgpu_wddm_d3d12.dll')).Hash -ne '768CD0033B43A5EEBCC43BF2FA4B2B099745D81761CE0358C5B3754ACE081004'){throw 'Artifact hash'}
if((Get-FileHash (Join-Path $d 'adapter-kmt-probe.exe')).Hash -ne '7977C238F30957E94C66B1CC309ACC0CCC6DCE440078EFD4AE4DE512A7382415'){throw 'Artifact hash'}

if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'STOP'}
$p=Start-Process -FilePath "$d\adapter-kmt-probe.exe" -ArgumentList "$d\amdgpu_wddm_d3d12.dll" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$d\stdout.txt" -RedirectStandardError "$d\stderr.txt"
$handle=$p.Handle
if(!$p.WaitForExit(10000)){$p.Kill();$null=$p.WaitForExit(3000);throw 'Probe timeout'}
$p.Refresh()
if($p.ExitCode -ne 0){throw "Admission failed: $($p.ExitCode)"}
