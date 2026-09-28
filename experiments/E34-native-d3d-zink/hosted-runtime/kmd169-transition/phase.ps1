# Must be launched through bounded-child.exe. No standalone blocking-time guarantee.
param([Parameter(Mandatory)][ValidateSet('Capture','Quiesce','Disable','Install','Configure','Enable','Verify')][string]$Phase,
 [Parameter(Mandatory)][ValidateSet('candidate','restore')][string]$Arm,
 [Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Receipt,
 [Parameter(Mandatory)][long]$ChildDeadline)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\durable.ps1"
. "$PSScriptRoot\verify-cpu.ps1"
. "$PSScriptRoot\pnp-idle.ps1"
$directoryPath=[IO.Path]::GetFullPath($Directory)
if(!$directoryPath.StartsWith('C:\BC250\m13\kmd169-', [StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected trial directory'}
$out=$directoryPath
$start=Join-Path $out "$Receipt-start.json";$done=Join-Path $out "$Receipt-done.json"
if(Test-Path $start){throw 'Stage already attempted; inspect its state'}
$boundary=Get-Content "$out\boundary.json" -Raw|ConvertFrom-Json
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
if($boundary.boot -ne $boot -or $boundary.machine -ne $env:COMPUTERNAME){throw 'Boot/host changed'}
$limit=if($Arm -eq 'candidate'){110}else{170}
if($boundary.frequency -ne [Diagnostics.Stopwatch]::Frequency -or [long]$boundary.qpc -le 0){throw 'Invalid monotonic boundary'}
$elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-[long]$boundary.qpc)/[double]$boundary.frequency
if($boundary.frequency -ne [Diagnostics.Stopwatch]::Frequency -or $elapsed -lt 0 -or $elapsed -ge $limit){throw 'Stage deadline expired'}
if($Arm -eq 'candidate' -and (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
Write-DurableText $start (@{pid=$PID;phase=$Phase;arm=$Arm;qpc=[Diagnostics.Stopwatch]::GetTimestamp();utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
$manifest=Get-Content "$out\package-hashes.json" -Raw|ConvertFrom-Json
foreach($label in @('candidate169','rollback166')){
 foreach($file in $manifest.$label.PSObject.Properties){
  if((Get-FileHash -LiteralPath (Join-Path "$out\$label" $file.Name)).Hash -ne $file.Value){throw 'Package identity mismatch'}
 }
}
if($Phase -eq 'Capture'){
 if($Arm -ne 'candidate'){throw 'Capture only before candidate'}
 $raw=& "$PSScriptRoot\preflight166.ps1"|Out-String
 $baseline=$raw|ConvertFrom-Json
 $gpu=@(Get-PnpDevice -Class Display|Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpu.Count -ne 1){throw 'Ambiguous adapter'}
 $baseline|Add-Member -NotePropertyName instance -NotePropertyValue $gpu[0].InstanceId
 $hardware=@((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data)[0]
 $baseline|Add-Member -NotePropertyName hardware -NotePropertyValue $hardware
 Write-DurableText "$out\baseline.json" ($baseline|ConvertTo-Json -Depth 10)
}else{
 $saved=Get-Content "$out\baseline.json" -Raw|ConvertFrom-Json
 if($saved.kmd_sha256 -ne $manifest.rollback166.'bc250kmd.sys' -or $saved.health -notmatch 'version=0x000700A6 flags=15'){throw 'Baseline witness invalid'}
 $gpu=Get-PnpDevice -InstanceId $saved.instance
 $hardware=@((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data)[0]
 if($hardware -ne $saved.hardware -or $hardware -notlike 'PCI\VEN_1002&DEV_13FE*'){throw 'Adapter identity changed'}
 $reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
 $version=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
 $expectedVersion=if($Arm -eq 'candidate'){'0.7.169.1'}else{'0.7.166.1'}
 $label=if($Arm -eq 'candidate'){'candidate169'}else{'rollback166'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 $actual=(Get-FileHash -LiteralPath $image).Hash
 if($actual -notin @($manifest.candidate169.'bc250kmd.sys',$manifest.rollback166.'bc250kmd.sys')){throw 'Unknown current SYS'}
 if($Arm -eq 'restore' -and $Phase -ne 'Quiesce' -and !(Test-Path "$out\restore-admitted.json")){throw 'Missing PnP restore admission'}
 $pnp=Get-KmdPnpIdle
 Write-DurableText "$out\$Receipt-pnp.json" ($pnp|ConvertTo-Json -Depth 5)
 Assert-KmdPnpIdleResult $pnp
 $problem=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
 switch($Phase){
  'Quiesce' {
   if($Arm -ne 'restore'){throw 'Quiesce is a restore admission phase'}
   if($problem -notin @(0,22)){throw 'Device not in a known enabled/disabled state'}
   if(!(Test-Path "$out\candidate-tree-closed.json")){throw 'Candidate tree closure not witnessed'}
   Write-DurableText "$out\restore-admitted.json" (@{qpc=[Diagnostics.Stopwatch]::GetTimestamp();problem=$problem;pnp=$pnp}|ConvertTo-Json -Depth 6)
  }
  'Disable' {
   if($Arm -eq 'candidate' -and ($version -ne '0.7.166.1' -or $actual -ne $manifest.rollback166.'bc250kmd.sys')){throw 'Candidate admission requires exact166'}
   if(!(Test-Path "$out\mutation-start.json")){Write-DurableText "$out\mutation-start.json" (@{qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|ConvertTo-Json)}
   if($problem -ne 22){& pnputil.exe /disable-device $gpu.InstanceId|Out-Null;if($LASTEXITCODE -ne 0){throw 'Disable failed'}}
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Disable not observed'}
  }
  'Install' {
   if($problem -ne 22){throw 'Install requires disabled adapter'}
   if($Arm -eq 'candidate'){
    & pnputil.exe /add-driver "$out\candidate169\bc250kmd.inf" /install|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Candidate install failed'}
   }else{
    $env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
    Add-Type 'using System;using System.Runtime.InteropServices;public static class KmdRestore169{[DllImport("newdev.dll",CharSet=CharSet.Unicode,SetLastError=true)][return:MarshalAs(UnmanagedType.Bool)]public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr p,string h,string i,uint f,[MarshalAs(UnmanagedType.Bool)]out bool reboot);}'
    $reboot=$false
    if(![KmdRestore169]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero,$hardware,"$out\rollback166\bc250kmd.inf",1,[ref]$reboot)){throw 'Forced166 restore failed'}
    if($reboot){throw 'Restore requests reboot; explicit recovery required'}
   }
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data -ne $expectedVersion){throw 'Installed version mismatch'}
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Unexpected automatic enable'}
  }
  'Configure' {
   if($problem -ne 22 -or $version -ne $expectedVersion -or $actual -ne $manifest.$label.'bc250kmd.sys'){throw 'Configure requires expected disabled driver'}
   $class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
   New-ItemProperty $class -Name UserModeDriverName -PropertyType MultiString -Value @($saved.umd_registration) -Force|Out-Null
   New-ItemProperty $class -Name VulkanDriverName -PropertyType MultiString -Value @($saved.icd_registration) -Force|Out-Null
   foreach($item in $saved.parameters.PSObject.Properties){New-ItemProperty $reg -Name $item.Name -Value $item.Value.value -PropertyType $item.Value.kind -Force|Out-Null}
   Set-DurablePresentGates 0
  }
  'Enable' {
   if($problem -ne 22 -or $version -ne $expectedVersion -or $actual -ne $manifest.$label.'bc250kmd.sys'){throw 'Enable identity mismatch'}
   if(!(Test-Path "$out\$Arm-configure-done.json")){throw 'Missing configuration receipt'}
   & pnputil.exe /enable-device $gpu.InstanceId|Out-Null
   if($LASTEXITCODE -ne 0){throw 'Enable failed'}
  }
  'Verify' {
   if($version -ne $expectedVersion -or $actual -ne $manifest.$label.'bc250kmd.sys' -or $gpu.Status -ne 'OK'){throw 'Active identity mismatch'}
   $umd=(Get-FileHash C:\BC250\m11\resource-close\bc250d3d.dll).Hash
   $icd=(Get-FileHash C:\BC250\m10\wsi-final\vulkan_radeon.dll).Hash
   if($umd -ne $saved.umd_sha256 -or $icd -ne $saved.icd_sha256){throw 'CPU baseline files changed'}
   & C:\BC250\bc250rd\bc250rd_cli.exe clock-check 1000 820|Out-Null
   if($LASTEXITCODE -ne 0){throw 'Clock control failed'}
   $class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
   $ready=Wait-KmdCpuBaseline -Saved $saved -Deadline ($ChildDeadline-[Diagnostics.Stopwatch]::Frequency) -Read {
   $registration=Get-ItemProperty $class
   $parameters=Get-ItemProperty $reg
   return @{
    umd_registration=@($registration.UserModeDriverName)
    icd_registration=@($registration.VulkanDriverName)
    parameters=$parameters
    dwm=@(Get-Process dwm | ForEach-Object {
     @{pid=$_.Id;start=$_.StartTime.ToUniversalTime().ToString('o');modules=@($_.Modules |
      Where-Object {$_.ModuleName -match 'bc250|vulkan_radeon'} |
      ForEach-Object {@{name=$_.ModuleName;sha256=(Get-FileHash -LiteralPath $_.FileName).Hash}})}
    })
   }
   } -Record {
    param($sample)
    Write-DurableText "$out\$Receipt-ready-$($sample.attempt).json" ($sample|ConvertTo-Json)
   }
   $observed=$ready.observed
   Write-DurableText "$out\$Receipt-readiness.json" ($ready|ConvertTo-Json -Depth 10)
   $health=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read|Out-String
   $abi=if($Arm -eq 'candidate'){'0x000700A9'}else{'0x000700A6'}
   if($LASTEXITCODE -ne 0 -or $health -notmatch "version=$abi flags=15"){throw 'Independent health not ready'}
   $info=& C:\BC250\m8\bc250kmd_cli.exe info|Out-String
   if($LASTEXITCODE -ne 0 -or $info -notmatch $abi -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded KMD identity mismatch'}
   Write-DurableText "$out\$Receipt-cpu.json" ($observed|ConvertTo-Json -Depth 8)
   Write-DurableText "$out\$Receipt-info.txt" $info
   Write-DurableText "$out\$Receipt-health.txt" $health
  }
 }
}
Write-DurableText $done (@{phase=$Phase;arm=$Arm;utc=[DateTime]::UtcNow.ToString('o');qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|ConvertTo-Json)
