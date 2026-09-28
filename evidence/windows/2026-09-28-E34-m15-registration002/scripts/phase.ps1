param([ValidateSet('Capture','Install','Cpu','Gpu','Restore','Verify','VerifyRestored')][string]$Phase)
$ErrorActionPreference='Stop';$d=$PSScriptRoot
. "$d\registration.ps1"
. "$d\durable.ps1"
function Open-GpuKey {
 $gpu=@(Get-PnpDevice -Class Display|Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpu.Count -ne 1){throw 'Adapter identity admission'}
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
function Restart-Adapter($tag){
 $gpu=@(Get-PnpDevice -Class Display|Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpu.Count -ne 1){throw 'Adapter identity admission'}
 Write-DurableText "$d\$tag-requested.json" (@{utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
 # pnputil completion is required; timeout is never treated as a completed PnP transition.
 $p=Start-Process -FilePath "$env:windir\System32\pnputil.exe" -ArgumentList @('/restart-device',('"'+$gpu[0].InstanceId+'"')) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$d\$tag.out" -RedirectStandardError "$d\$tag.err"
 $handle=$p.Handle
 if(!$p.WaitForExit(25000)){$p.Kill();throw 'PnP action completion unknown'}
 $p.Refresh();Write-DurableText "$d\$tag-result.json" (@{exit_code=$p.ExitCode;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
 if($p.ExitCode -ne 0){throw 'PnP restart did not complete successfully; no automatic OS reboot'}
}
if($Phase -in @('Capture','Install','Cpu','Gpu')){
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
 if($Phase -eq 'Install'){Restart-Adapter 'install-pnp'}
 elseif(Test-Path "$d\install-pnp-requested.json"){Restart-Adapter 'restore-pnp'}
}
if($Phase -eq 'Cpu'){
 if((Probe 'installed-name') -notmatch 'version=3 status=00000000 basename=amdgpu_wddm_d3d12.dll'){throw 'DX12 registration not visible through KMT'}
 . "$d\confirmed-present-start171.ps1"
 $watch=[Diagnostics.Stopwatch]::StartNew();$confirmed=$false
 while($watch.Elapsed.TotalSeconds -lt 75){
  $health=(& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read|Out-String)
  Add-Content "$d\confirmation.txt" ([DateTime]::UtcNow.ToString('o')+"`n"+$health)
  if($LASTEXITCODE -eq 0){try{$gate=Get-ConfirmedPresentStart -Health $health -ElapsedSeconds $watch.Elapsed.TotalSeconds;if($gate.launch){$confirmed=$true;break}}catch{}}
  Start-Sleep -Seconds 2
 }
 if(!$confirmed){throw 'Restarted adapter did not confirm CPU presentation'}
}
if($Phase -eq 'Gpu'){
 $p=Start-Process -FilePath "$d\amdgpu_wddm_d3d12_queue.exe" -ArgumentList '--lab' -PassThru -WindowStyle Hidden -RedirectStandardOutput "$d\runtime.out" -RedirectStandardError "$d\runtime.err"
 $handle=$p.Handle
 if(!$p.WaitForExit(10000)){$p.Kill();throw 'Runtime timeout'}
 $p.Refresh();$out=Get-Content "$d\runtime.out" -Raw;$err=Get-Content "$d\runtime.err" -Raw
 Write-DurableText "$d\runtime-result.json" (@{exit_code=$p.ExitCode;functional=($p.ExitCode -eq 0);open_adapter=($err -match 'd3d12-ddi OpenAdapter12 0')}|ConvertTo-Json)
 # This diagnostic UMD still refuses GetCaps; require actual system-runtime entry.
 if($out -notmatch 'runtime=system32/d3d12.dll adapter=BC-250' -or $err -notmatch 'd3d12-ddi OpenAdapter12 0'){throw 'System runtime did not enter the registered UMD'}
}
if($Phase -in @('Verify','VerifyRestored')){
 & "$d\preflight171.ps1" | Set-Content "$d\$Phase-state.json"
 $a=Get-Content "$d\before.json" -Raw|ConvertFrom-Json;$b=Get-Content "$d\$Phase-state.json" -Raw|ConvertFrom-Json
 $plan=Get-Content "$d\plan.json" -Raw|ConvertFrom-Json
 $desired=if($Phase -eq 'Verify'){$plan.after}else{$plan.before}
 if($a.boot -ne $b.boot -or !(Test-M15NamesEqual $desired $b.umd_registration) -or !(Test-M15NamesEqual $a.icd_registration $b.icd_registration)){throw 'Unexpected postflight state'}
 $expected=if($Phase -eq 'Verify'){'version=3 status=00000000 basename=amdgpu_wddm_d3d12.dll'}else{'version=3 status=c000000d basename=<unavailable>'}
 if((Probe ($Phase+'-name')) -notmatch $expected){throw 'Effective DX12 registration mismatch'}
}
Write-DurableText "$d\$Phase-done.json" (@{phase=$Phase;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
