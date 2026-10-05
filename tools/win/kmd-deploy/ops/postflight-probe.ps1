# Read-only independent postflight after an accepted promotion and Cleanup. Launched by ops/postflight-run.ps1
# inside a bounded child. Compares the live lab with this attempt's own capture and pins; changes nothing.
param([Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\registration.ps1"
. "$PSScriptRoot\hang-detector.ps1"
. "$PSScriptRoot\confirmed-present-start.ps1"
$Directory = [IO.Path]::GetFullPath($Directory)
if ($Directory -notmatch $KmdDirectoryPattern) { throw 'Unexpected directory' }
$result = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); read_only = $true }
$result.stop = [bool](Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop
if ($result.stop) { throw 'Owner STOP requested' }
$saved = Get-Content "$Directory\baseline.json" -Raw | ConvertFrom-Json
$boundary = Get-Content "$Directory\boundary.json" -Raw | ConvertFrom-Json
$watch = Get-Content "$Directory\watch-result.json" -Raw | ConvertFrom-Json
$hashes = Get-Content "$Directory\package-hashes.json" -Raw | ConvertFrom-Json
$result.watch = @{ status = $watch.status; candidate_retained = $watch.candidate_retained; elapsed = $watch.elapsed; logging_restored = $watch.logging_restored }
if ($watch.status -ne 'closed' -or $watch.candidate_retained -ne $true) { throw 'Postflight applies to a retained candidate only' }
$gpu = @(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if ($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK') { throw 'Expected one healthy GPU' }
$result.version = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if ($result.version -ne $KmdCandidateVersion) { throw 'Candidate version not active' }
$image = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if ($image.StartsWith('\SystemRoot\')) { $image = Join-Path $env:windir $image.Substring(12) }
if ($image.StartsWith('\??\')) { $image = $image.Substring(4) }
$result.kmd_sha256 = (Get-FileHash -LiteralPath $image).Hash
if ($result.kmd_sha256 -ne $hashes.$KmdCandidateLabel.'bc250kmd.sys') { throw 'Candidate SYS not active' }
$activeInf = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
if ($activeInf -notmatch '^oem[0-9]+\.inf$' -or (Get-FileHash -LiteralPath "$env:windir\INF\$activeInf").Hash -ne $hashes.$KmdCandidateLabel.'bc250kmd.inf') { throw 'Active INF identity mismatch' }
$result.umd_sha256 = (Get-FileHash -LiteralPath $KmdDesktopUmdPath).Hash
$result.icd_sha256 = (Get-FileHash -LiteralPath $KmdIcdPath).Hash
if ($result.umd_sha256 -ne $saved.umd_sha256 -or $result.icd_sha256 -ne $saved.icd_sha256) { throw 'UMD/ICD baseline changed' }
$driverKey = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Class\' + $driverKey)
if (!$classKey) { throw 'Class key missing' }
try { $result.graphics_registration = Read-KmdGraphicsRegistration $classKey } finally { $classKey.Dispose() }
Assert-KmdGraphicsRegistration $saved.graphics_registration $result.graphics_registration
$result.registration_files = Get-KmdRegistrationFiles $result.graphics_registration
Assert-KmdRegistrationFiles $saved.registration_files $result.registration_files
$parametersKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters')
if (!$parametersKey) { throw 'Driver registry key absent' }
try { $result.hang_detector = Read-KmdHangDetector $parametersKey } finally { $parametersKey.Dispose() }
Assert-KmdHangDetectorClosed $result.hang_detector
# After Cleanup the values equal the capture, absence included; before it, the candidate's explicit 0.
$result.hang_detector_as_captured = Test-KmdHangDetectorEqual $result.hang_detector $saved.hang_detector
if (!$result.hang_detector_as_captured) { throw 'Hang detector not restored as captured; run Cleanup or RestoreDetector' }
$result.boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
if ($result.boot -ne $boundary.boot) { throw 'Boot changed since the transition: recovery, not acceptance' }
$result.dwm = @(Get-Process dwm | ForEach-Object { @{ pid = $_.Id; start = $_.StartTime.ToString('o'); modules = @($_.Modules | Where-Object { $_.ModuleName -match 'bc250|vulkan_radeon' } | ForEach-Object { @{ name = $_.ModuleName; sha256 = (Get-FileHash -LiteralPath $_.FileName).Hash } }) } })
$result.test_processes = @(Get-Process | Where-Object { $_.ProcessName -match 'deqp|vkcube|witcher3|PresentMon|state-control|texture-control|cross-process|flip-control|gfx-blt-control|runtime-audit-control|redirblt-probe|bc250dxvk_engine_test|d3d11bench|debug-child|amdgpu_wddm_d3d12' } | Select-Object ProcessName,Id)
$result.running_tasks = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' } | Select-Object TaskName,State)
$result.watch_task = if (Get-ScheduledTask -TaskName $KmdTaskName -ErrorAction SilentlyContinue) { 'Present' } else { 'Missing' }
$result.legacy_clock_state = [string](Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State
if ($result.legacy_clock_state -ne 'Disabled') { throw 'Legacy writer enabled' }
$result.info = (& C:\BC250\m8\bc250kmd_cli.exe info | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.info -notmatch $KmdCandidateAbi -or $result.info -notmatch 'FULL WDDM TABLE') { throw 'Unexpected loaded KMD' }
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
$result.confirmed = Get-ConfirmedPresentStart -Health $result.health -ElapsedSeconds 0 -Abi $KmdCandidateAbi
if (!$result.confirmed.launch) { throw 'Confirmed CPU baseline required' }
if ($result.clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000) { throw 'Clock operating point mismatch' }
if ($result.dwm.Count -ne 1 -or @($result.dwm[0].modules | Where-Object { $_.sha256 -eq $result.umd_sha256 }).Count -ne 1) { throw 'One CPU DWM with the baseline UMD required' }
foreach ($name in @('EnableGpuPresentBlit','EnableCddDwmInterop','UnconfirmedStarts')) { if ($parameters.Contains($name) -and $parameters[$name].value -ne 0) { throw ('Baseline gate/guard changed: ' + $name) } }
foreach ($name in @($saved.parameters.PSObject.Properties | Where-Object { $_.Name -notin (@('UnconfirmedStarts','LastStage','StageHistory') + @(Get-KmdHangDetectorNames)) })) {
 if (!$parameters.Contains($name.Name) -or $parameters[$name.Name].value -ne $name.Value.value) { throw ('Driver parameter differs from capture: ' + $name.Name) }
}
if ($result.test_processes.Count) { throw 'Competing test process' }
(($result.running_tasks | ForEach-Object { $_.TaskName }) -join ", ") | Set-Content -LiteralPath "C:/BC250/tmp/running.txt"
unning.txt
if ($result.watch_task -ne 'Missing') { throw 'Watch task still registered; Cleanup first' }
if ($result.driver_log -notmatch 'CDD interop0 GPU Present gate0 identity probe1') { throw 'Latched baseline gates missing' }
if ($result.driver_log -match 'hang: detector ARMED') { throw 'Hang detector armed' }
$result | ConvertTo-Json -Depth 8
