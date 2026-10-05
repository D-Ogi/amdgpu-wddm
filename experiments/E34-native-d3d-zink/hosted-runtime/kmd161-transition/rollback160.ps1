# Explicit recovery only. Never automatically invoked on an SSH observation timeout.
$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07161'
$pkg=Join-Path $out 'rollback160'
$expected='8E676C810190EF38BACF3E8332EDC2760E07844FD779E18CB32B22D24AC90218'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) { throw 'Owner STOP requested' }
if ((Get-FileHash (Join-Path $pkg 'bc250kmd.sys')).Hash -ne $expected) { throw 'Rollback SYS mismatch' }
$hashes=Get-Content (Join-Path $out 'package-hashes.json') -Raw | ConvertFrom-Json
foreach($file in $hashes.rollback160.PSObject.Properties){
 if((Get-FileHash (Join-Path $pkg $file.Name)).Hash -ne $file.Value){throw 'Rollback package mismatch'}
}
$saved=Get-Content (Join-Path $out 'parameters-before.json') -Raw | ConvertFrom-Json
$registration=Get-Content (Join-Path $out 'registration-before.json') -Raw | ConvertFrom-Json
if($saved.UnconfirmedStarts.value -ne 0 -or $saved.EnableFullWddm.value -ne 2){throw 'Unexpected saved guard configuration'}
if((Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State -ne 'Disabled'){throw 'Legacy writer active'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1){throw 'Ambiguous recovery target'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if($version -ne '0.7.161.1'){throw 'Recovery requires installed161; inspect state manually'}
$hardware=@((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data)[0]
if($hardware -notlike 'PCI\VEN_1002&DEV_13FE*'){throw 'Unexpected hardware ID'}
# SDK10.0.26100 um/newdev.h: INSTALLFLAG_FORCE=1. Reuse the previously used
# forced downgrade API; pnputil /add-driver alone will not force an older version.
$env:TEMP='C:\BC250\tmp'; $env:TMP=$env:TEMP
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Kmd161Recovery {
 [DllImport("newdev.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)]
 public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent,string hardwareId,string inf,uint flags,[MarshalAs(UnmanagedType.Bool)]out bool reboot);
}
'@
Start-Transcript -Path (Join-Path $out ('rollback-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')+'.log')) -NoClobber | Out-Null
try {
 $problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
 if($problem -ne 22){
  & pnputil.exe /disable-device $gpu[0].InstanceId
  if($LASTEXITCODE -ne 0){throw 'Recovery disable failed'}
 }
 $problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
 if($problem -ne 22){throw 'Recovery device is not disabled'}
 $reboot=$false
 $ok=[Kmd161Recovery]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero,$hardware,(Join-Path $pkg 'bc250kmd.inf'),1,[ref]$reboot)
 $errorCode=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
 if(-not $ok){throw "Forced rollback failed Win32=$errorCode"}
 if($reboot){throw 'Reboot required: inspect and preserve evidence before recovery restart'}
 if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Unexpected automatic enable during rollback'}
 $class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
 foreach($name in @('UserModeDriverName','VulkanDriverName')){
  New-ItemProperty $class -Name $name -PropertyType MultiString -Value @($registration.$name) -Force | Out-Null
 }
 $reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
 foreach($item in $saved.PSObject.Properties){
  New-ItemProperty $reg -Name $item.Name -Value $item.Value.value -PropertyType $item.Value.kind -Force | Out-Null
 }
 foreach($probeName in @('EnableGpuPresentBlit','EnableHandleIdentityProbe')){
  if(-not $saved.PSObject.Properties[$probeName]){Remove-ItemProperty $reg -Name $probeName -ErrorAction SilentlyContinue}
 }
 & pnputil.exe /enable-device $gpu[0].InstanceId
 if($LASTEXITCODE -ne 0){throw 'Recovery enable failed'}
 & "$out\wait-rollback160.ps1"
 & C:\BC250\m8\bc250kmd_cli.exe log | Out-File (Join-Path $out 'rollback-early-driver.log')
 $info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
 if($LASTEXITCODE -ne 0 -or $info -notmatch '0x000700A0' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Recovered KMD not active/full'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 if((Get-FileHash $image).Hash -ne $expected){throw 'Recovered SYS identity mismatch'}
 if((Get-PnpDevice -InstanceId $gpu[0].InstanceId).Status -ne 'OK'){throw 'Recovered device unhealthy'}
 & C:\BC250\bc250rd\bc250rd_cli.exe clock-check 1000 820
 if($LASTEXITCODE -ne 0){throw 'Recovered clock control failed'}
 'Recovered160: independent health, DWM modules and paging/render regression still required.'
} finally { Stop-Transcript | Out-Null }
