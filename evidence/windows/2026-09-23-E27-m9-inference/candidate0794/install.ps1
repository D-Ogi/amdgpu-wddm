$ErrorActionPreference='Stop'
$pkg='C:\BC250\m9\candidate0794\package-umd'
$expected='D25EEAA8BD1E754B3BB3A7B781051C85B4F7E54777721FAD1A1E22AA250A84DD'
$hash=(Get-FileHash -LiteralPath (Join-Path $pkg 'bc250kmd.sys')).Hash
if ($hash -ne $expected) { throw 'Candidate SYS hash mismatch' }
'candidate_sha256='+$hash
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gates=@('EnableFullWddm','EnableMmio','EnableMmioWrite','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','EnableVidPnFlip','EnableDcnWrite')
$values=Get-ItemProperty $reg
foreach ($n in $gates) { if ($values.$n -ne 0) { throw ("Gate is not closed: "+$n) } }
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'boot_before='+$boot
$raw=& 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
$raw
if ($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'No temperature' }
if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
& pnputil.exe /add-driver (Join-Path $pkg 'bc250kmd.inf') /install
$code=$LASTEXITCODE
'pnputil_exit='+$code
if ($code -ne 0) { throw ("Install requires inspection, code="+$code+"; no reboot requested") }
Start-Sleep -Seconds 5
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if ($gpu.Count -ne 1) { throw 'Expected one target display device' }
'device_status='+$gpu[0].Status
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$ver
if ($ver -ne '0.7.94.1' -or $gpu[0].Status -ne 'OK') { throw 'Candidate not active/healthy' }
$svc=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd'
$path=$svc.ImagePath
if ($path.StartsWith('\SystemRoot\')) { $path=Join-Path $env:windir $path.Substring(12) }
if ($path.StartsWith('\??\')) { $path=$path.Substring(4) }
$installed=(Get-FileHash -LiteralPath $path).Hash
'installed_sha256='+$installed
if ($installed -ne $expected) { throw 'Installed SYS mismatch' }
$values=Get-ItemProperty $reg
foreach ($n in $gates) { if ($values.$n -ne 0) { throw ("Gate changed: "+$n) } }
'closed_gates_verified'
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'dwm_processes='+(@(Get-Process dwm -ErrorAction SilentlyContinue).Count)
'display_only_install_passed'
