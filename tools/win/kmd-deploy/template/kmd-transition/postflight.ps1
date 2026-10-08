# Read-only independent postflight after an accepted promotion and Cleanup. Launched by ops/postflight-run.ps1
# inside a bounded child. Compares the live lab with this attempt's own capture and pins; changes nothing.
param([Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\registration.ps1"
. "$PSScriptRoot\hang-detector.ps1"
. "$PSScriptRoot\parameters.ps1"
. "$PSScriptRoot\confirmed-present-start.ps1"
. "$PSScriptRoot\verify-cpu.ps1"
. "$PSScriptRoot\release.ps1"
$Directory = [IO.Path]::GetFullPath($Directory)
if ($Directory -notmatch $KmdDirectoryPattern) { throw 'Unexpected directory' }
$result = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); read_only = $true }
$result.stop = [bool](Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop
if ($result.stop) { throw 'Owner STOP requested' }
# Every KMD query goes through the installed release's client, with its usage read once.
$cli = Resolve-KmdClient (Get-KmdReleaseClientPath) @('info', 'health read', 'clock read', 'log summary')
$result.cli = $cli
$result.cli_sha256 = (Get-FileHash -LiteralPath $cli).Hash
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
$result.info = (& $cli info | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.info -notmatch $KmdCandidateAbi -or $result.info -notmatch 'FULL WDDM TABLE') { throw 'Unexpected loaded KMD' }
$result.health = (& $cli health read | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Health query failed' }
$result.clock = (& $cli clock read | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Clock query failed' }
$result.temperature = (& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.temperature -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'Temperature unavailable' }
if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
$parameters = [ordered]@{}
$key = Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
foreach ($name in $key.GetValueNames()) { $parameters[$name] = @{ value = $key.GetValue($name); kind = $key.GetValueKind($name).ToString() } }
$result.parameters = $parameters
$result.driver_log = (& $cli log summary | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Log query failed' }
$result.confirmed = Get-ConfirmedPresentStart -Health $result.health -ElapsedSeconds 0 -Abi $KmdCandidateAbi
if (!$result.confirmed.launch) { throw 'Confirmed CPU baseline required' }
if ($result.clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000) { throw 'Clock operating point mismatch' }
if ($result.dwm.Count -ne 1 -or @($result.dwm[0].modules | Where-Object { $_.sha256 -eq $result.umd_sha256 }).Count -ne 1) { throw 'One CPU DWM with the baseline UMD required' }
# The captured desktop (preflight's `desktop`; none before the router: CPU UMD alone, switches 0).
$desktop = Get-KmdSavedDesktop $saved
$result.desktop = $desktop
if ($null -ne $desktop.modules) {
 $seen = @($result.dwm[0].modules | ForEach-Object { [string]$_.sha256 } | Sort-Object -Unique)
 if (($seen -join ',') -cne ($desktop.modules -join ',') -or @($result.dwm[0].modules).Count -ne $desktop.modules.Count) { throw 'DWM modules differ from the captured desktop route' }
}
if ($parameters.Contains('UnconfirmedStarts') -and $parameters['UnconfirmedStarts'].value -ne 0) { throw 'Baseline gate/guard changed: UnconfirmedStarts' }
foreach ($name in @('EnableGpuPresentBlit','EnableCddDwmInterop')) { if (!$parameters.Contains($name) -or $parameters[$name].value -ne $desktop.switches) { throw ('Baseline gate/guard changed: ' + $name) } }
# Operator and installer values must equal the capture; what the KMD writes about its own starts is reported only.
# The interop record is the KMD's too (Get-KmdInteropWrittenNames): reported here, its latch checked below.
$interopNames = @(Get-KmdInteropWrittenNames)
$compare = Compare-KmdCapturedParameters -Saved $saved.parameters -Live $parameters -Skip (@(Get-KmdHangDetectorNames) + $interopNames)
$result.parameters_kmd_written = $compare.kmd_written
$result.parameters_added = $compare.added
$result.interop_record = [ordered]@{}
foreach ($name in $interopNames) { $result.interop_record[$name] = [ordered]@{ captured = $saved.parameters.$name.value; live = $(if ($parameters.Contains($name)) { $parameters[$name].value } else { $null }) } }
if ($desktop.switches -eq 1 -and ($result.interop_record.InteropLastState.live -ne 0x303 -or $result.interop_record.InteropLastReason.live -ne 0)) { throw 'Interop latch is not 0x303 reason 0' }
if ($compare.differs.Count) { throw ('Driver parameter differs from capture: ' + ($compare.differs -join ', ')) }
if ($result.test_processes.Count) { throw 'Competing test process' }
if (@($result.running_tasks | Where-Object { $_.TaskName -notin @('BC250 monitor overlay','BC250 net watchdog') }).Count) { throw ('Competing test task: ' + (@($result.running_tasks | ForEach-Object TaskName) -join ', ')) }
if ($result.watch_task -ne 'Missing') { throw 'Watch task still registered; Cleanup first' }
if ($result.driver_log -notmatch ('CDD interop{0} GPU Present gate{0} identity probe1' -f $desktop.switches)) { throw 'Latched baseline gates missing' }
if ($result.driver_log -match 'hang: detector ARMED') { throw 'Hang detector armed' }
$result | ConvertTo-Json -Depth 8
