# Must be launched through bounded-child.exe. No standalone blocking-time guarantee.
param([Parameter(Mandatory)][ValidateSet('Capture','Quiesce','Rebind','Disable','Install','Configure','Enable','Verify','CleanupPackage')][string]$Phase,
 [Parameter(Mandatory)][ValidateSet('candidate','restore')][string]$Arm,
 [Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Receipt,
 [Parameter(Mandatory)][long]$ChildDeadline)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\durable.ps1"
. "$PSScriptRoot\transition-policy.ps1"
. "$PSScriptRoot\verify-cpu.ps1"
. "$PSScriptRoot\pnp-idle.ps1"
. "$PSScriptRoot\install-observation.ps1"
. "$PSScriptRoot\package-cleanup.ps1"
. "$PSScriptRoot\registration.ps1"
. "$PSScriptRoot\hang-detector.ps1"
. "$PSScriptRoot\parameters.ps1"
. "$PSScriptRoot\release.ps1"
$directoryPath=[IO.Path]::GetFullPath($Directory)
if($directoryPath -notmatch $KmdDirectoryPattern){throw 'Unexpected trial directory'}
$out=$directoryPath
$mode=Get-KmdTransitionPolicy $out
$start=Join-Path $out "$Receipt-start.json";$done=Join-Path $out "$Receipt-done.json"
if(Test-Path $start){throw 'Stage already attempted; inspect its state'}
$boundary=Get-Content "$out\boundary.json" -Raw|ConvertFrom-Json
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
if($boundary.boot -ne $boot -or $boundary.machine -ne $env:COMPUTERNAME){throw 'Boot/host changed'}
$limit=if($Arm -eq 'candidate'){if($mode -eq $KmdDeployMode){122}else{87}}else{170}
if($boundary.frequency -ne [Diagnostics.Stopwatch]::Frequency -or [long]$boundary.qpc -le 0){throw 'Invalid monotonic boundary'}
$elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-[long]$boundary.qpc)/[double]$boundary.frequency
if($boundary.frequency -ne [Diagnostics.Stopwatch]::Frequency -or $elapsed -lt 0 -or $elapsed -ge $limit){throw 'Stage deadline expired'}
if($Arm -eq 'candidate' -and (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
Write-DurableText $start (@{pid=$PID;phase=$Phase;arm=$Arm;qpc=[Diagnostics.Stopwatch]::GetTimestamp();utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)
$manifest=Get-Content "$out\package-hashes.json" -Raw|ConvertFrom-Json
foreach($label in @($KmdCandidateLabel,$KmdRollbackLabel)){
 foreach($file in $manifest.$label.PSObject.Properties){
  if((Get-FileHash -LiteralPath (Join-Path "$out\$label" $file.Name)).Hash -ne $file.Value){throw 'Package identity mismatch'}
 }
}
if($manifest.$KmdRollbackLabel.'bc250kmd.sys' -ne $KmdRollbackSysSha256){throw 'Rollback package pin mismatch'}
if($Phase -eq 'Capture'){
 if($Arm -ne 'candidate'){throw 'Capture only before candidate'}
 # Package staging is separate; reject before device disable if either is missing.
 $publishedPackages=@(Get-KmdPublishedPackages)
 foreach($packageLabel in @($KmdCandidateLabel,$KmdRollbackLabel)){
  $registered=Select-KmdRegisteredPackage $publishedPackages $manifest.$packageLabel.'bc250kmd.inf'
  Write-DurableText "$out\$Receipt-registered-$packageLabel.json" ($registered|ConvertTo-Json)
 }
 $raw=& "$PSScriptRoot\preflight.ps1"|Out-String
 $baseline=$raw|ConvertFrom-Json
 $gpu=@(Get-PnpDevice -Class Display|Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpu.Count -ne 1){throw 'Ambiguous adapter'}
 $baseline|Add-Member -NotePropertyName instance -NotePropertyValue $gpu[0].InstanceId
 $hardware=@((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data)[0]
 $baseline|Add-Member -NotePropertyName hardware -NotePropertyValue $hardware
 Write-DurableText "$out\baseline.json" ($baseline|ConvertTo-Json -Depth 10)
}else{
 $saved=Get-Content "$out\baseline.json" -Raw|ConvertFrom-Json
 if($saved.kmd_sha256 -ne $manifest.$KmdRollbackLabel.'bc250kmd.sys' -or $saved.health -notmatch ('version='+[regex]::Escape($KmdRollbackAbi)+' flags=15')){throw 'Baseline witness invalid'}
 Assert-KmdHangDetectorBaseline $saved.hang_detector
 [void](Assert-KmdGraphicsRegistrationBaseline $saved.graphics_registration)
 $gpu=Get-PnpDevice -InstanceId $saved.instance
 $hardware=@((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data)[0]
 if($hardware -ne $saved.hardware -or $hardware -notlike 'PCI\VEN_1002&DEV_13FE*'){throw 'Adapter identity changed'}
 $reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
 $version=$null
 try{$version=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction Stop).Data}catch{if($Phase -notin @('Quiesce','Rebind')){throw}}
 $expectedVersion=if($Arm -eq 'candidate' -and $mode -ne $KmdSameMode){$KmdCandidateVersion}else{$KmdRollbackVersion}
 $label=if($Arm -eq 'candidate'){$KmdCandidateLabel}else{$KmdRollbackLabel}
 # Start state of the hang detector for this arm: candidate explicitly closed, restore exactly as captured.
 $detectorExpected=if($Arm -eq 'candidate'){Get-KmdCandidateHangDetector $saved.hang_detector}else{ConvertTo-KmdHangDetectorState $saved.hang_detector}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 $actual=(Get-FileHash -LiteralPath $image).Hash
 if($actual -notin @($manifest.$KmdCandidateLabel.'bc250kmd.sys',$manifest.$KmdRollbackLabel.'bc250kmd.sys')){throw 'Unknown current SYS'}
 if($Arm -eq 'restore' -and $Phase -ne 'Quiesce' -and !(Test-Path "$out\restore-admitted.json")){throw 'Missing PnP restore admission'}
 $pnp=Get-KmdPnpIdle -Deadline $ChildDeadline
 Write-DurableText "$out\$Receipt-pnp.json" ($pnp|ConvertTo-Json -Depth 5)
 Assert-KmdPnpIdleResult $pnp
 $problem=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
 switch($Phase){
  'Quiesce' {
   if($Arm -ne 'restore'){throw 'Quiesce is a restore admission phase'}
   Assert-KmdRestorableProblem $problem
   if(!(Test-Path "$out\candidate-tree-closed.json")){throw 'Candidate tree closure not witnessed'}
   Write-DurableText "$out\restore-admitted.json" (@{qpc=[Diagnostics.Stopwatch]::GetTimestamp();problem=$problem;pnp=$pnp}|ConvertTo-Json -Depth 6)
  }
  'Rebind' {
   if($mode -ne $KmdSameMode -or $Arm -ne 'restore'){throw 'Rebind is only for same-package control recovery'}
   if($manifest.$KmdCandidateLabel.'bc250kmd.sys' -ne $manifest.$KmdRollbackLabel.'bc250kmd.sys'){throw 'Same-package control identity mismatch'}
   # Reuse successful binding only after a completed disabled-install receipt.
   $reuse=(Test-Path "$out\candidate-install-done.json") -and $version -eq $KmdRollbackVersion -and $problem -eq 22
   if(!$reuse){
    Write-DurableText "$out\$Receipt-fallback-start.json" (@{qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|ConvertTo-Json)
    $env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
    Add-Type 'using System;using System.Runtime.InteropServices;public static class SameBaselineRecovery{[DllImport("newdev.dll",CharSet=CharSet.Unicode,SetLastError=true)][return:MarshalAs(UnmanagedType.Bool)]public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr p,string h,string i,uint f,[MarshalAs(UnmanagedType.Bool)]out bool reboot);}'
    $reboot=$false
    $ok=[SameBaselineRecovery]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero,$saved.hardware,"$out\$KmdRollbackLabel\bc250kmd.inf",1,[ref]$reboot)
    $errorCode=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-DurableText "$out\$Receipt-fallback-result.json" (@{ok=$ok;reboot=$reboot;error=$errorCode;observation=(Get-KmdInstallObservation $gpu.InstanceId)}|ConvertTo-Json -Depth 10)
    if(!$ok -or $reboot){throw 'Fallback rebind failed or requires explicit reboot recovery'}
   }
   & pnputil.exe /disable-device $gpu.InstanceId|Out-Null
   if($LASTEXITCODE -ne 0){throw 'Recovery disable failed'}
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data -ne $KmdRollbackVersion -or
      (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Recovery binding/disable not witnessed'}
  }
  'Disable' {
   if($Arm -eq 'candidate' -and ($version -ne $KmdRollbackVersion -or $actual -ne $manifest.$KmdRollbackLabel.'bc250kmd.sys')){throw 'Candidate admission requires the exact baseline'}
   if(!(Test-Path "$out\mutation-start.json")){Write-DurableText "$out\mutation-start.json" (@{qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|ConvertTo-Json)}
   if($problem -ne 22){& pnputil.exe /disable-device $gpu.InstanceId|Out-Null;if($LASTEXITCODE -ne 0){throw 'Disable failed'}}
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Disable not observed'}
  }
  'Install' {
   if($problem -ne 22){throw 'Install requires disabled adapter'}
   $registered=Select-KmdRegisteredPackage @(Get-KmdPublishedPackages) $manifest.$label.'bc250kmd.inf'
   $publishedInf=Join-Path "$env:windir\INF" $registered.name
   $inspection=@(& "$out\select-driver.exe" --inspect-store $gpu.InstanceId $publishedInf)
   if($LASTEXITCODE -ne 0){throw 'Registered INF inspection failed'}
   Write-DurableText "$out\$Receipt-registered-inspection.jsonl" ($inspection -join "`n")
   $storeRows=@($inspection|ForEach-Object {$_|ConvertFrom-Json}|Where-Object {$_.resolved_store_inf})
   if($storeRows.Count -ne 1 -or (Get-FileHash -LiteralPath $storeRows[0].resolved_store_inf).Hash -ne $manifest.$label.'bc250kmd.inf'){
    throw 'Registered store INF hash mismatch'
   }
   if((Get-FileHash -LiteralPath $publishedInf).Hash -ne $manifest.$label.'bc250kmd.inf'){throw 'Published INF changed'}
   $installObservation=Invoke-KmdObservedInstall -Read {Get-KmdInstallObservation $gpu.InstanceId} -Save {
    param($stage,$value)
    Write-DurableText "$out\$Receipt-install-$stage.json" ($value|ConvertTo-Json -Depth 10)
   } -Install {
    & "$out\select-driver.exe" --install-deferred $gpu.InstanceId $publishedInf $expectedVersion | Out-Host
    return $LASTEXITCODE
   }
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data -ne $expectedVersion){throw 'Installed version mismatch'}
   if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Unexpected automatic enable'}
  }
  'Configure' {
   if($problem -ne 22 -or $version -ne $expectedVersion -or $actual -ne $manifest.$label.'bc250kmd.sys'){throw 'Configure requires expected disabled driver'}
   $driverKey=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
   $classKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(('SYSTEM\CurrentControlSet\Control\Class\'+$driverKey),$true)
   if(!$classKey){throw 'Installed class key missing'}
   try {
    # The complete captured registration (four-entry UserModeDriverName included), in its captured kinds;
    # values of these families that the install added are removed. Set-KmdGraphicsRegistration reads back.
    $registrationResult=Set-KmdGraphicsRegistration -Key $classKey -Saved $saved.graphics_registration
    Write-DurableText "$out\$Receipt-registration.json" ($registrationResult|ConvertTo-Json -Depth 8)
    # The two lists the CPU acceptance compares, re-read entry by entry against their separate capture.
    foreach($entry in @(@('UserModeDriverName','umd_registration'),@('VulkanDriverName','icd_registration'))){
     $value=[string[]]$saved.($entry[1])
     if(!$value.Count -or @($value|Where-Object {[string]::IsNullOrWhiteSpace($_)}).Count){throw 'Invalid saved graphics registration'}
     $readback=[string[]]@($classKey.GetValue($entry[0]))
     if($readback.Count -ne $value.Count){throw 'Class registration readback mismatch'}
     for($i=0;$i -lt $value.Count;$i++){if($readback[$i] -cne $value[$i]){throw 'Class registration readback mismatch'}}
    }
   }finally{$classKey.Dispose()}
   # The interop record (interop.c) is the KMD's own: writing a captured InteropSession back would plant a session
   # marker, and the stop already wrote InteropLastEnd. Only the operator's switches are restored (just below).
   $skip=@('UnconfirmedStarts','LastStage','StageHistory')+@(Get-KmdHangDetectorNames)+@(Get-KmdInteropWrittenNames)
   # What the install did to Parameters, before this write-back undoes it: an INF install used to close every gate
   # the INF names (BD-091), and the release INF now writes each at its released value with NOCLOBBER. The
   # write-back of the capture covers both shapes, so this receipt is what says which one the lab had.
   $installed=[ordered]@{}
   $installedKey=Get-Item $reg
   foreach($name in $installedKey.GetValueNames()){$installed[$name]=@{value=$installedKey.GetValue($name);kind=$installedKey.GetValueKind($name).ToString()}}
   Write-DurableText "$out\$Receipt-parameters-after-install.json" ((Compare-KmdCapturedParameters -Saved $saved.parameters -Live $installed -Skip @(Get-KmdInteropWrittenNames))|ConvertTo-Json -Depth 6)
   foreach($item in $saved.parameters.PSObject.Properties){if($item.Name -in $skip){continue};New-ItemProperty $reg -Name $item.Name -Value $item.Value.value -PropertyType $item.Value.kind -Force|Out-Null}
   # A fresh start budget for the driver this phase configures (guard.c). The INF writes this 0 at every install and
   # deploy-candidate.ps1 writes it by hand for the same reason: a count left at BC250_MAX_UNCONFIRMED_STARTS
   # refuses the next start with Code 43, which inside the bounded window costs the whole attempt (BD-090).
   New-ItemProperty $reg -Name UnconfirmedStarts -Value 0 -PropertyType DWord -Force|Out-Null
   # Detector values are written with absence as a state and read back before the device can start.
   $parametersKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters',$true)
   if(!$parametersKey){throw 'Driver registry key absent'}
   try {$detector=Set-KmdHangDetector -Key $parametersKey -Desired $detectorExpected}finally{$parametersKey.Dispose()}
   Write-DurableText "$out\$Receipt-detector.json" ($detector|ConvertTo-Json -Depth 6)
   # The captured desktop's switches, written and read back: 0 for the CPU desktop as before, 1 under the router
   # (a 0 here latches 0x000 at Enable and strands the GPU route, as the promotion of revision 182 did).
   Set-DurablePresentGates (Get-KmdSavedDesktop $saved).switches
  }
  'Enable' {
   if($problem -ne 22 -or $version -ne $expectedVersion -or $actual -ne $manifest.$label.'bc250kmd.sys'){throw 'Enable identity mismatch'}
   if(!(Test-Path "$out\$Arm-configure-done.json")){throw 'Missing configuration receipt'}
   & pnputil.exe /enable-device $gpu.InstanceId|Out-Null
   if($LASTEXITCODE -ne 0){throw 'Enable failed'}
  }
  'CleanupPackage' {
   if($Arm -ne 'restore' -or $version -ne $KmdRollbackVersion -or $actual -ne $manifest.$KmdRollbackLabel.'bc250kmd.sys' -or $problem -ne 0){throw 'Package cleanup requires the active exact baseline'}
   if(!(Test-Path "$out\restore-verify-done.json")){throw 'CPU rollback verification missing'}
   $activeInf=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
   if($activeInf -notmatch '^oem[0-9]+\.inf$' -or (Get-FileHash -LiteralPath "$env:windir\INF\$activeInf").Hash -ne $manifest.$KmdRollbackLabel.'bc250kmd.inf'){throw 'Active rollback INF identity mismatch'}
   $packages=@(Select-KmdCandidatePackages -Packages @(Get-KmdPublishedPackages) -ExpectedInfHash $manifest.$KmdCandidateLabel.'bc250kmd.inf' -ActiveInf $activeInf)
   Write-DurableText "$out\$Receipt-before.json" (@{active=$activeInf;candidates=$packages}|ConvertTo-Json -Depth 5)
   foreach($package in $packages){
    # Recheck immediately before the non-forced removal; never uninstall a device.
    if((Get-FileHash -LiteralPath "$env:windir\INF\$($package.name)").Hash -ne $manifest.$KmdCandidateLabel.'bc250kmd.inf'){throw 'Published INF identity changed'}
    if((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data -ieq $package.name){throw 'Candidate became active'}
    & pnputil.exe /delete-driver $package.name|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Candidate package removal failed'}
   }
   $remaining=@(Select-KmdCandidatePackages -Packages @(Get-KmdPublishedPackages) -ExpectedInfHash $manifest.$KmdCandidateLabel.'bc250kmd.inf' -ActiveInf $activeInf)
   if($remaining.Count){throw 'Candidate package remains staged'}
  }
  'Verify' {
   if($version -ne $expectedVersion -or $actual -ne $manifest.$label.'bc250kmd.sys' -or $gpu.Status -ne 'OK'){throw 'Active identity mismatch'}
   # Every KMD query of this phase goes through the installed release's client, with its usage read once. The
   # client is the release's, never a staged copy: a candidate's own client would answer about itself.
   $cli=Resolve-KmdClient (Get-KmdReleaseClientPath) @('info','health read','health confirm','log')
   $umd=(Get-FileHash -LiteralPath $KmdDesktopUmdPath).Hash
   $icd=(Get-FileHash -LiteralPath $KmdIcdPath).Hash
   if($umd -ne $saved.umd_sha256 -or $icd -ne $saved.icd_sha256){throw 'CPU baseline files changed'}
   & C:\BC250\bc250rd\bc250rd_cli.exe clock-check 1000 820|Out-Null
   if($LASTEXITCODE -ne 0){throw 'Clock control failed'}
   $driverKey=(Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
   $class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+$driverKey
   $readCpu={
   $registration=Get-ItemProperty $class
   $parameters=Get-ItemProperty $reg
   return @{
    umd_registration=@($registration.UserModeDriverName)
    icd_registration=@($registration.VulkanDriverName)
    parameters=(Select-KmdCpuParameters $parameters)
    dwm=@(Get-Process dwm | ForEach-Object {
     @{pid=$_.Id;start=$_.StartTime.ToUniversalTime().ToString('o');modules=@($_.Modules |
      Where-Object {$_.ModuleName -match 'bc250|vulkan_radeon'} |
      ForEach-Object {@{name=$_.ModuleName;sha256=(Get-FileHash -LiteralPath $_.FileName).Hash}})}
    })
   }
   }
   $abi=if($Arm -eq 'candidate' -and $mode -ne $KmdSameMode){$KmdCandidateAbi}else{$KmdRollbackAbi}
   $script:lastHealthText=''
   $readHealth={
    $text=& $cli health read|Out-String
    $script:lastHealthText="exit $LASTEXITCODE`r`n$text"
    if($LASTEXITCODE -ne 0){throw 'Independent health query failed'}
    Get-KmdReadyHealth $text $abi
   }
   try {
    $ready=Wait-KmdCpuBaseline -Saved $saved -AllowUnconfirmed -Deadline ($ChildDeadline-5*[Diagnostics.Stopwatch]::Frequency) -Read $readCpu -ReadHealth $readHealth -Record {
     param($sample)
     Write-DurableText "$out\$Receipt-ready-$($sample.attempt).json" ($sample|ConvertTo-Json -Depth 5)
    }
   } catch {
    # A start that never gets ready leaves no other trace of the driver's own view once the restore unloads it:
    # keep the last raw health answer and the KMD log ring (best effort, the original error is rethrown).
    try {
     Write-DurableText "$out\$Receipt-health-last.txt" ([string]$script:lastHealthText)
     $failLog=& $cli log|Out-String
     Write-DurableText "$out\$Receipt-kmdlog-failed.txt" ("exit $LASTEXITCODE`r`n"+$failLog)
    } catch {}
    throw
   }
   $startHealth=$ready.health
   $observed=$ready.observed
   Write-DurableText "$out\$Receipt-readiness.json" ($ready|ConvertTo-Json -Depth 10)
   $health=& $cli health read|Out-String
   if($LASTEXITCODE -ne 0){throw 'Independent health query failed'}
   $before=Get-KmdReadyHealth $health $abi
   Assert-KmdSameHealthStart $startHealth $before
   if($Arm -eq 'candidate'){Assert-KmdFreshWork $before}
   Write-DurableText "$out\$Receipt-health-before.txt" $health
   $info=& $cli info|Out-String
   if($LASTEXITCODE -ne 0 -or $info -notmatch $abi -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded KMD identity mismatch'}
   # Registration, its files and the detector, checked while the ready interval runs, not after it.
   $classKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Class\'+$driverKey)
   if(!$classKey){throw 'Class key missing'}
   try {$graphics=Read-KmdGraphicsRegistration $classKey}finally{$classKey.Dispose()}
   Assert-KmdGraphicsRegistration $saved.graphics_registration $graphics
   $files=Get-KmdRegistrationFiles $graphics
   Assert-KmdRegistrationFiles $saved.registration_files $files
   $parametersKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters')
   if(!$parametersKey){throw 'Driver registry key absent'}
   try {$detector=Read-KmdHangDetector $parametersKey}finally{$parametersKey.Dispose()}
   if(!(Test-KmdHangDetectorEqual $detector $detectorExpected)){throw 'Hang detector state mismatch'}
   Assert-KmdHangDetectorClosed $detector
   # hang.c logs this line when it arms; the ring holds this start's lines unless it wrapped (the registry
   # readback above is the primary witness).
   $log=& $cli log|Out-String
   if($LASTEXITCODE -ne 0){throw 'Log query failed'}
   if($log -match 'hang: detector ARMED'){throw 'Hang detector armed at start'}
   # The ring's lines carry milliseconds since the driver load: where a slow start spent its time (the
   # revision 179 deploy attempt of 2026-09-30 needed 15 s from enable to ready, its rollback 3 s) is read
   # from here afterwards.
   Write-DurableText "$out\$Receipt-kmdlog.txt" $log
   Write-DurableText "$out\$Receipt-registration.json" (@{graphics=$graphics;files=$files;detector=$detector;detector_expected=$detectorExpected}|ConvertTo-Json -Depth 8)
   $requireConfirmed=($Arm -eq 'restore' -or $mode -eq $KmdDeployMode)
   if($requireConfirmed){
    $before=Wait-KmdConfirmEligible -Before $startHealth -Read $readHealth -Deadline ($ChildDeadline-5*[Diagnostics.Stopwatch]::Frequency) -Record {
     param($sample)
     Write-DurableText "$out\$Receipt-health-wait-$($sample.attempt).json" ($sample|ConvertTo-Json -Depth 5)
    }
   }
   if($requireConfirmed -and $before.flags -eq 7){
    Assert-KmdConfirmEligible $before
    $confirmed=& $cli health confirm $before.generation $before.epoch|Out-String
    if($LASTEXITCODE -ne 0){throw 'Checked health confirmation failed'}
    Assert-KmdConfirmedHealth $before (Get-KmdReadyHealth $confirmed $abi)
    Write-DurableText "$out\$Receipt-health-confirm.txt" $confirmed
   }
   $health=& $cli health read|Out-String
   if($LASTEXITCODE -ne 0){throw 'Final health read failed'}
   $finalHealth=Get-KmdReadyHealth $health $abi
   Assert-KmdSameHealthStart $startHealth $finalHealth
   Assert-KmdFreshWork $finalHealth
   if($requireConfirmed){Assert-KmdConfirmedHealth $before $finalHealth}
   Write-DurableText "$out\$Receipt-health-acceptance.json" (@{scope=$(if($Arm -eq 'candidate'){if($requireConfirmed){'candidate-confirmed'}else{'candidate-ready-only'}}else{'restored-confirmed'});health=$finalHealth}|ConvertTo-Json -Depth 4)
   $observed=& $readCpu
   Assert-KmdCpuBaseline $saved $observed -AllowUnconfirmed:(!$requireConfirmed)
   Write-DurableText "$out\$Receipt-cpu.json" ($observed|ConvertTo-Json -Depth 8)
   Write-DurableText "$out\$Receipt-info.txt" $info
   Write-DurableText "$out\$Receipt-health.txt" $health
  }
 }
}
Write-DurableText $done (@{phase=$Phase;arm=$Arm;utc=[DateTime]::UtcNow.ToString('o');qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|ConvertTo-Json)
