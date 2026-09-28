$ErrorActionPreference='Stop'
$gpu=@(Get-PnpDevice -Class Display -PresentOnly|Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1){throw 'Ambiguous adapter'}
$instance=$gpu[0].InstanceId
$published=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_DriverInfPath).Data
$published=Join-Path "$env:windir\INF" $published
$external='C:\BC250\m13\kmd169-stage005\rollback166\bc250kmd.inf'
if((Get-FileHash $published).Hash -ne (Get-FileHash $external).Hash){throw 'INF content differs'}
$before=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ProblemCode).Data
$version=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_DriverVersion).Data
@{phase='before';problem=$before;version=$version;inf_sha256=(Get-FileHash $published).Hash;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress
foreach($entry in @(@('--inspect',$external),@('--inspect',$published),@('--inspect-store',$published))){
 @{mode=$entry[0];input_inf=$entry[1]}|ConvertTo-Json -Compress
 & "$PSScriptRoot\select-driver.exe" $entry[0] $instance $entry[1]
 if($LASTEXITCODE -ne 0){throw "Inspect failed: $LASTEXITCODE"}
}
$after=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ProblemCode).Data
@{phase='after';problem=$after;version=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_DriverVersion).Data;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress
if($after -ne $before){throw 'Problem state changed during read-only inspection'}
