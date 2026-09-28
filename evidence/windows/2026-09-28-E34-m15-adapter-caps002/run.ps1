$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$hashes=Get-Content "$PSScriptRoot\binaries.json" -Raw|ConvertFrom-Json
foreach($p in $hashes.PSObject.Properties){if((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $p.Name)).Hash -cne $p.Value){throw "Artifact hash mismatch: $($p.Name)"}}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'STOP'}
$env:VKD3D_DEBUG='info'
$p=Start-Process -FilePath "$PSScriptRoot\adapter-caps-probe.exe" -ArgumentList @("`"$PSScriptRoot\amdgpu_wddm_vkd3d.dll`"","`"$PSScriptRoot\amdgpu_wddm_radv.dll`"") -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput "$PSScriptRoot\stdout.txt" -RedirectStandardError "$PSScriptRoot\stderr.txt"
$handle=$p.Handle
$p.WaitForExit();$p.Refresh();$probeExit=$p.ExitCode
if($probeExit -ne 0){throw "Adapter query exit $probeExit"}
$out=Get-Content "$PSScriptRoot\stdout.txt"
if(!($out -cmatch '^PASSED$') -or !($out -cmatch '^QueryAdapterCaps hr=00000000 closed_without_device_callbacks=1 queue_calls=0$')){throw 'Adapter query witness missing'}
[ordered]@{passed=$true;scope='hosted adapter-only ABI1.2 QueryAdapterCaps; no VkDevice or native D3D12CreateDevice';binaries=$hashes;caps=@($out|Where-Object {$_ -match '^caps:'})}|ConvertTo-Json -Depth 4|Set-Content "$PSScriptRoot\admission.json"
