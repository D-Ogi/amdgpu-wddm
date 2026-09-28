$ErrorActionPreference = 'Stop'
$result = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); read_only = $true }
$result.stop = [bool](Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop
if ($result.stop) { throw 'Owner STOP requested' }
$gpu = @(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if ($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK') { throw 'Expected one healthy GPU' }
$result.version = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if ($result.version -ne '0.7.169.1') { throw 'Baseline version changed' }
$image = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if ($image.StartsWith('\SystemRoot\')) { $image = Join-Path $env:windir $image.Substring(12) }
if ($image.StartsWith('\??\')) { $image = $image.Substring(4) }
$result.kmd_sha256 = (Get-FileHash -LiteralPath $image).Hash
if ($result.kmd_sha256 -ne 'AF715A5641E577E1D3023F22A38026D2E0189802F5E7146C7A868E9CD6D9D987') { throw 'Baseline SYS changed' }
$result.umd_sha256 = (Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
$result.icd_sha256 = (Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
if ($result.umd_sha256 -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA' -or $result.icd_sha256 -ne 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157') { throw 'UMD/ICD baseline changed' }
$class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$registration = Get-ItemProperty $class
$result.umd_registration = @($registration.UserModeDriverName)
$result.icd_registration = @($registration.VulkanDriverName)
$result.boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
$result.dwm = @(Get-Process dwm | ForEach-Object { @{ pid = $_.Id; start = $_.StartTime.ToString('o'); modules = @($_.Modules | Where-Object { $_.ModuleName -match 'bc250|vulkan_radeon' } | ForEach-Object { @{ name = $_.ModuleName; sha256 = (Get-FileHash -LiteralPath $_.FileName).Hash } }) } })
$result.test_processes = @(Get-Process | Where-Object { $_.ProcessName -match 'deqp|vkcube|witcher3|PresentMon|state-control|texture-control|cross-process|flip-control|gfx-blt-control|runtime-audit-control|redirblt-probe' } | Select-Object ProcessName,Id)
$result.running_tasks = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' } | Select-Object TaskName,State)
$result.legacy_clock_state = [string](Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State
if ($result.legacy_clock_state -ne 'Disabled') { throw 'Legacy writer enabled' }
$result.info = (& C:\BC250\m8\bc250kmd_cli.exe info | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.info -notmatch '0x000700A9' -or $result.info -notmatch 'FULL WDDM TABLE') { throw 'Unexpected loaded KMD' }
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
$result.driver_log = (& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Log query failed' }
. "$PSScriptRoot\confirmed-present-start.ps1"
$result.confirmed=Get-ConfirmedPresentStart -Health $result.health -ElapsedSeconds 0
if(!$result.confirmed.launch){throw 'Confirmed CPU baseline required'}
if($result.clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock operating point mismatch'}
if($result.dwm.Count -ne 1 -or @($result.dwm[0].modules|Where-Object {$_.sha256 -eq $result.umd_sha256}).Count -ne 1){throw 'One CPU DWM with the baseline UMD required'}
foreach($name in @('EnableGpuPresentBlit','EnableCddDwmInterop')){if($parameters.Contains($name) -and $parameters[$name].value -ne 0){throw 'Desktop gates must remain off'}}
if($result.test_processes.Count){throw 'Competing test process'}
$allowedTasks=@('BC250 monitor overlay','BC250 net watchdog','BC250-G0-DwmRun049')
if(@($result.running_tasks|Where-Object {$_.TaskName -notin $allowedTasks}).Count){throw 'Competing test task'}
if($result.driver_log -notmatch 'CDD interop0 GPU Present gate0 identity probe1'){throw 'Latched baseline gates missing'}
$result | ConvertTo-Json -Depth 8
