$ErrorActionPreference='Stop'
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1) { throw 'Expected one adapter' }
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if($version -ne '0.7.122.1') { throw 'Unexpected version' }
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-ItemProperty $reg | Select-Object Enable*,LastStage,StageHistory,UnconfirmedStarts | Format-List
& 'C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe' clock-check 1000 820
if($LASTEXITCODE -ne 0) { throw 'Clock preflight failed' }
$raw=& 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
$raw
if($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'No temperature' }
if([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
$gates=@('EnableRlcReloadReset','EnableFullWddm','EnableMmio','EnableMmioWrite','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','EnableVidPnFlip','EnableDcnWrite','EnablePresentBlit')
foreach($n in $gates) { New-ItemProperty $reg -Name $n -Value 0 -PropertyType DWord -Force | Out-Null }
$values=Get-ItemProperty $reg
foreach($n in $gates) { if($values.$n -ne 0) { throw ('Gate not closed: '+$n) } }
'closed_gates_verified'
New-ItemProperty $reg -Name UnconfirmedStarts -Value 0 -PropertyType DWord -Force | Out-Null
'pnp_restart_begin='+(Get-Date).ToString('o')
& pnputil.exe /restart-device $gpu[0].InstanceId
$code=$LASTEXITCODE
'pnputil_exit='+$code
if($code -ne 0) { throw 'Inspect restart result; no automatic retry or reboot' }
Get-PnpDevice -InstanceId $gpu[0].InstanceId | Select-Object Status,Problem | Format-List
Get-ItemProperty $reg | Select-Object Enable*,LastStage,StageHistory,UnconfirmedStarts | Format-List
& 'C:\BC250\m8\bc250kmd_cli.exe' info
'info_exit='+$LASTEXITCODE
& 'C:\BC250\m8\bc250kmd_cli.exe' log
'log_exit='+$LASTEXITCODE
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'recovery_finished'
