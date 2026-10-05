$ErrorActionPreference='Stop'
$out='C:\BC250\m9\candidate07145\coldboot-01'
$cli='C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe'
'now='+(Get-Date).ToString('s')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1){throw 'Unexpected adapter count'}
'gpu_status='+$gpu[0].Status
'version='+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'problem='+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
'kmd_sha256='+(Get-FileHash -LiteralPath $image).Hash
& $cli health read
'health_exit='+$LASTEXITCODE
& $cli clock read
'clock_exit='+$LASTEXITCODE
Get-Process dwm,bc250mon -ErrorAction SilentlyContinue | ForEach-Object {$_.ProcessName+' pid='+$_.Id+' start='+$_.StartTime.ToString('s')}
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object EnableFullWddm,UnconfirmedStarts,LastStage | Format-List
Get-ScheduledTask -TaskName 'BC250 cold145 health recorder' | Select-Object State
Get-ScheduledTaskInfo -TaskName 'BC250 cold145 health recorder' | Select-Object LastRunTime,LastTaskResult
if(Test-Path "$out\startup-health.log"){Get-Content "$out\startup-health.log" -Tail 20}
& $cli log | Out-File "$out\postboot-driver.log" -Encoding UTF8
'postboot_record_complete'
