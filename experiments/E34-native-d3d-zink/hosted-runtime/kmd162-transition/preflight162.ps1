$ErrorActionPreference = 'Stop'
$result = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); read_only = $true }
$result.stop = [bool](Invoke-RestMethod http://127.0.0.1:2250/flags).stop
if ($result.stop) { throw 'Owner STOP requested' }
$gpu = @(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if ($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK') { throw 'Expected one healthy GPU' }
$result.version = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if ($result.version -ne '0.7.161.1') { throw 'Baseline version changed' }
$image = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if ($image.StartsWith('\SystemRoot\')) { $image = Join-Path $env:windir $image.Substring(12) }
if ($image.StartsWith('\??\')) { $image = $image.Substring(4) }
$result.kmd_sha256 = (Get-FileHash -LiteralPath $image).Hash
if ($result.kmd_sha256 -ne '40F7916FE24957666F392D5913EE70C7DCFEB1A23A121C39E6DB0ACE907B6FF1') { throw 'Baseline SYS changed' }
$result.umd_sha256 = (Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
$result.icd_sha256 = (Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
if ($result.umd_sha256 -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA' -or $result.icd_sha256 -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D') { throw 'UMD/ICD baseline changed' }
$class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$registration = Get-ItemProperty $class
$result.umd_registration = @($registration.UserModeDriverName)
$result.icd_registration = @($registration.VulkanDriverName)
$result.boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
$result.dwm = @(Get-Process dwm | ForEach-Object { @{ pid = $_.Id; start = $_.StartTime.ToString('o'); modules = @($_.Modules | Where-Object { $_.ModuleName -match 'bc250|vulkan_radeon' } | ForEach-Object { @{ name = $_.ModuleName; sha256 = (Get-FileHash -LiteralPath $_.FileName).Hash } }) } })
$result.test_processes = @(Get-Process | Where-Object { $_.ProcessName -match 'deqp|vkcube|witcher3|PresentMon|state-control|texture-control|cross-process|flip-control|gfx-blt-control' } | Select-Object ProcessName,Id)
$result.running_tasks = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' } | Select-Object TaskName,State)
$result.legacy_clock_state = [string](Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State
if ($result.legacy_clock_state -ne 'Disabled') { throw 'Legacy writer enabled' }
$result.info = (& C:\BC250\m8\bc250kmd_cli.exe info | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.info -notmatch '0x000700A1' -or $result.info -notmatch 'FULL WDDM TABLE') { throw 'Unexpected loaded KMD' }
$result.health = (& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Health query failed' }
$result.clock = (& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Clock query failed' }
$result.temperature = (& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.temperature -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'Temperature unavailable' }
if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
$parameters = [ordered]@{}
$key = Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
foreach ($name in $key.GetValueNames()) { $parameters[$name] = @{ value = $key.GetValue($name); kind = $key.GetValueKind($name).ToString() } }
$result.parameters = $parameters
$result.driver_log = (& C:\BC250\m8\bc250kmd_cli.exe log | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Log query failed' }
if ($result.health -notmatch 'version=0x000700A1 flags=15') { throw 'Baseline not confirmed healthy' }
if ($result.test_processes.Count) { throw 'Concurrent lab workload' }
foreach($name in 'EnableGpuPresentBlit','EnableCddDwmInterop','UnconfirmedStarts') {
 if($parameters[$name].value -ne 0){throw ('Baseline gate/guard changed: '+$name)}
}
$result | ConvertTo-Json -Depth 8
