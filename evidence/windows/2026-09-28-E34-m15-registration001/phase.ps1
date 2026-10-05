param([ValidateSet('Capture','Install','Cpu','Gpu','Restore','Verify')][string]$Phase)
$ErrorActionPreference='Stop';$d=$PSScriptRoot
. "$d\registration.ps1"
. "$d\durable.ps1"
function Open-GpuKey {
 $gpu=@(Get-PnpDevice -Class Display|Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Adapter admission'}
 $sub='SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
 return [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sub,$true)
}
function Probe($tag){
 $process=Start-Process -FilePath "$d\adapter-kmt-probe.exe" -ArgumentList "$d\amdgpu_wddm_d3d12.dll" -PassThru -WindowStyle Hidden -RedirectStandardOutput "$d\$tag.out" -RedirectStandardError "$d\$tag.err"
 $handle=$process.Handle
 if(!$process.WaitForExit(8000)){$process.Kill();throw 'Probe timeout'}
 $process.Refresh();if($process.ExitCode -ne 0){throw 'Probe failure'}
 return Get-Content "$d\$tag.out" -Raw
}
if($Phase -in @('Capture','Cpu','Gpu')){
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
}
if($Phase -eq 'Capture'){
 & "$d\preflight171.ps1" | Set-Content "$d\before.json"
 $key=Open-GpuKey
 try{
  $plan=New-M15RegistrationPlan -Before @($key.GetValue('UserModeDriverName')) -Kind ([string]$key.GetValueKind('UserModeDriverName')) -Candidate "$d\amdgpu_wddm_d3d12.dll"
  $expected=@('bc250umd.dll','C:\BC250\m11\resource-close\bc250d3d.dll','C:\BC250\m11\resource-close\bc250d3d.dll')
  if(!(Test-M15NamesEqual $plan.before $expected)){throw 'Baseline names mismatch'}
  Write-DurableText "$d\plan.json" ($plan|ConvertTo-Json -Depth 5)
 }finally{if($key){$key.Dispose()}}
 if((Probe 'baseline-name') -notmatch 'version=3 status=c000000d basename=<unavailable>'){throw 'Unexpected baseline DX12 name'}
}
if($Phase -in @('Install','Restore')){
 $plan=Get-Content "$d\plan.json" -Raw|ConvertFrom-Json
 $key=Open-GpuKey
 try{Set-M15Registration -Key $key -Plan $plan -Mode $Phase}finally{if($key){$key.Dispose()}}
}
if($Phase -eq 'Cpu'){
 if((Probe 'installed-name') -notmatch 'version=3 status=00000000 basename=amdgpu_wddm_d3d12.dll'){throw 'DX12 registration not visible through KMT; runtime probe skipped'}
}
if($Phase -eq 'Gpu'){
 $process=Start-Process -FilePath "$d\amdgpu_wddm_d3d12_queue.exe" -ArgumentList '--lab' -PassThru -WindowStyle Hidden -RedirectStandardOutput "$d\runtime.out" -RedirectStandardError "$d\runtime.err"
 $handle=$process.Handle
 if(!$process.WaitForExit(10000)){$process.Kill();throw 'Runtime timeout'}
 $process.Refresh()
 Write-DurableText "$d\runtime-result.json" (@{exit_code=$process.ExitCode;functional=($process.ExitCode -eq 0)}|ConvertTo-Json)
 # Preserve an expected unfinished-DDI failure as a measurement, not success.
 if($process.ExitCode -ne 0){throw "System runtime probe failed: $($process.ExitCode)"}
}
if($Phase -eq 'Verify'){
 & "$d\preflight171.ps1" | Set-Content "$d\after.json"
 $a=Get-Content "$d\before.json" -Raw|ConvertFrom-Json;$b=Get-Content "$d\after.json" -Raw|ConvertFrom-Json
 if($a.boot -ne $b.boot -or $a.confirmed.generation -ne $b.confirmed.generation -or $a.confirmed.epoch -ne $b.confirmed.epoch -or ($a.dwm|ConvertTo-Json -Depth 5 -Compress) -cne ($b.dwm|ConvertTo-Json -Depth 5 -Compress) -or !(Test-M15NamesEqual $a.umd_registration $b.umd_registration) -or !(Test-M15NamesEqual $a.icd_registration $b.icd_registration)){throw 'Baseline changed'}
 if((Probe 'restored-name') -notmatch 'version=3 status=c000000d basename=<unavailable>'){throw 'Effective DX12 name not restored'}
}
Write-DurableText "$d\$Phase-done.json" (@{phase=$Phase;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
