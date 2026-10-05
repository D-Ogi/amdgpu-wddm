# Startup-only acceptance: no full WDDM, engine RUN or OS reboot.
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$package='C:\BC250\m9\kmd0773'
$cli='C:\BC250\m8\bc250kmd_cli.exe'
$params='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$expected='6897960553995442D267A0A43B86220590E8873001B2BC63EFD37E8B0CD58888'
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$gates='EnableFullWddm','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','EnableGfx','EnablePsp','EnableIh','EnableGart','EnableVramWrite'
$p=Get-ItemProperty $params
foreach ($g in $gates) { if ($null -eq $p.$g -or $p.$g -ne 0) { throw "Gate not closed: $g" } }
$running=@(Get-ScheduledTask | Where-Object {$_.TaskName -like 'BC250*M9*' -and $_.State -eq 'Running'})
if ($running.Count) { throw 'M9 task is running' }
$raw=& 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
$raw
if ($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'Temperature unavailable' }
if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
if ((Get-FileHash "$package\bc250kmd.sys").Hash -ne $expected) { throw 'Package hash mismatch' }
& $cli info
if ($LASTEXITCODE -ne 0) { throw 'Initial driver unavailable' }
pnputil /add-driver "$package\bc250kmd.inf" /install
$installExit=$LASTEXITCODE
"INSTALL_EXIT=$installExit"
if ($installExit -eq 3010) { throw 'Installation requests restart; no automatic reboot' }
if ($installExit -ne 0) { throw 'Installation failed' }
Start-Sleep -Seconds 8
$gpu=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if ($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK') { throw 'GPU PnP status is not healthy' }
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if ($version -ne '0.7.73.1') { throw "Unexpected driver version: $version" }
$service=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd'
$image=$service.ImagePath -replace '^\\\?\?\\','' -replace '^\\SystemRoot', $env:SystemRoot
$image=[Environment]::ExpandEnvironmentVariables($image).Trim('"')
$installedHash=(Get-FileHash -LiteralPath $image).Hash
if ($installedHash -ne $expected) { throw 'Installed image hash mismatch' }
$p=Get-ItemProperty $params
foreach ($g in $gates) { if ($null -eq $p.$g -or $p.$g -ne 0) { throw "Gate changed: $g" } }
if ((Get-CimInstance Win32_OperatingSystem).LastBootUpTime -ne $boot) { throw 'Boot changed' }
$dwm=@(Get-Process dwm -ErrorAction SilentlyContinue)
if (-not $dwm.Count) { throw 'DWM absent' }
& $cli info
if ($LASTEXITCODE -ne 0) { throw 'New driver unavailable' }
& $cli confirm
if ($LASTEXITCODE -ne 0) { throw 'Confirmation failed' }
[ordered]@{Version=$version;Hash=$installedHash;Boot=$boot.ToString('s');PnpStatus=$gpu[0].Status;Dwm=@($dwm|Select-Object Id,Responding);Gates=(Get-ItemProperty $params|Select-Object $gates);UnconfirmedStarts=(Get-ItemProperty $params).UnconfirmedStarts;Scope='startup only, full WDDM disabled'}|ConvertTo-Json -Depth 4
