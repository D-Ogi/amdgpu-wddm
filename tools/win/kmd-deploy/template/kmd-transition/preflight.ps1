$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\registration.ps1"
. "$PSScriptRoot\hang-detector.ps1"
$result = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); read_only = $true }
$result.stop = [bool](Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop
if ($result.stop) { throw 'Owner STOP requested' }
$hashes = Get-Content "$PSScriptRoot\..\package-hashes.json" -Raw | ConvertFrom-Json
$rollback = $hashes.$KmdRollbackLabel
if ($rollback.'bc250kmd.sys' -ne $KmdRollbackSysSha256 -or $rollback.'bc250kmd.inf' -ne $KmdRollbackInfSha256 -or $rollback.'bc250kmd.cat' -ne $KmdRollbackCatSha256) { throw 'Rollback package pins changed' }
$gpu = @(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if ($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK') { throw 'Expected one healthy GPU' }
$result.version = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if ($result.version -ne $KmdRollbackVersion) { throw 'Baseline version changed' }
$image = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if ($image.StartsWith('\SystemRoot\')) { $image = Join-Path $env:windir $image.Substring(12) }
if ($image.StartsWith('\??\')) { $image = $image.Substring(4) }
$result.kmd_sha256 = (Get-FileHash -LiteralPath $image).Hash
if ($result.kmd_sha256 -ne $KmdRollbackSysSha256) { throw 'Baseline SYS changed' }
$result.umd_sha256 = (Get-FileHash -LiteralPath $KmdDesktopUmdPath).Hash
$result.icd_sha256 = (Get-FileHash -LiteralPath $KmdIcdPath).Hash
if ($result.umd_sha256 -ne $KmdDesktopUmdSha256 -or $result.icd_sha256 -ne $KmdIcdSha256) { throw 'UMD/ICD baseline changed' }
$driverKey = (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + $driverKey
$registration = Get-ItemProperty $class
$result.umd_registration = @($registration.UserModeDriverName)
$result.icd_registration = @($registration.VulkanDriverName)
# Fresh, complete capture of every graphics registration value in its stored kind; never an older baseline.
$classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Class\' + $driverKey)
if (!$classKey) { throw 'Class key missing' }
try {
 $result.graphics_registration = Read-KmdGraphicsRegistration $classKey
 $result.class_value_kinds = [ordered]@{}
 foreach ($name in @($classKey.GetValueNames() | Sort-Object)) { $result.class_value_kinds[$name] = [string]$classKey.GetValueKind($name) }
} finally { $classKey.Dispose() }
[void](Assert-KmdGraphicsRegistrationBaseline $result.graphics_registration)
foreach ($pair in @(@('UserModeDriverName','umd_registration'),@('VulkanDriverName','icd_registration'))) {
 $a = @($result.graphics_registration[$pair[0]].value); $b = @($result[$pair[1]])
 if ($a.Count -ne $b.Count) { throw 'Registration capture inconsistent' }
 for ($i = 0; $i -lt $a.Count; $i++) { if ($a[$i] -cne $b[$i]) { throw 'Registration capture inconsistent' } }
}
$result.registration_files = Get-KmdRegistrationFiles $result.graphics_registration
$result.boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
$result.dwm = @(Get-Process dwm | ForEach-Object { @{ pid = $_.Id; start = $_.StartTime.ToString('o'); modules = @($_.Modules | Where-Object { $_.ModuleName -match 'bc250|vulkan_radeon' } | ForEach-Object { @{ name = $_.ModuleName; sha256 = (Get-FileHash -LiteralPath $_.FileName).Hash } }) } })
$result.test_processes = @(Get-Process | Where-Object { $_.ProcessName -match 'deqp|vkcube|witcher3|PresentMon|state-control|texture-control|cross-process|flip-control|gfx-blt-control|runtime-audit-control|redirblt-probe|bc250dxvk_engine_test|d3d11bench|debug-child|amdgpu_wddm_d3d12' } | Select-Object ProcessName,Id)
$result.running_tasks = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' } | Select-Object TaskName,State)
$result.legacy_clock_state = [string](Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State
if ($result.legacy_clock_state -ne 'Disabled') { throw 'Legacy writer enabled' }
$result.info = (& C:\BC250\m8\bc250kmd_cli.exe info | Out-String)
if ($LASTEXITCODE -ne 0 -or $result.info -notmatch $KmdRollbackAbi -or $result.info -notmatch 'FULL WDDM TABLE') { throw 'Unexpected loaded KMD' }
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
# Presence and absence of both detector values; an armed baseline is refused, never carried into the candidate.
$parametersKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters')
if (!$parametersKey) { throw 'Driver registry key absent' }
try { $result.hang_detector = Read-KmdHangDetector $parametersKey } finally { $parametersKey.Dispose() }
Assert-KmdHangDetectorBaseline $result.hang_detector
$result.driver_log = (& C:\BC250\m8\bc250kmd_cli.exe log | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Log query failed' }
if ($result.health -notmatch ('version=' + [regex]::Escape($KmdRollbackAbi) + ' flags=15')) { throw 'Baseline not confirmed healthy' }
if ($result.clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000) { throw 'Clock operating point mismatch' }
if ($result.dwm.Count -ne 1 -or @($result.dwm[0].modules | Where-Object { $_.sha256 -eq $result.umd_sha256 }).Count -ne 1) { throw 'One CPU DWM with the baseline UMD required' }
# The desktop this attempt was frozen for (identity.ps1, from lab-baseline.json): exactly that module set in DWM,
# the switches at that value and, with a router, DWM held on the CPU route by DwmForceCpu 1 (`route.py NNN cpu`).
$desktopModules = @($KmdDesktopModules.Split(','))
$seen = @($result.dwm[0].modules | ForEach-Object { [string]$_.sha256 } | Sort-Object -Unique)
if (($seen -join ',') -cne ($desktopModules -join ',') -or @($result.dwm[0].modules).Count -ne $desktopModules.Count) { throw 'DWM modules differ from the frozen desktop route' }
$result.desktop = [ordered]@{ switches = [int]$KmdDesktopSwitches; modules = $desktopModules; router_key = $KmdDesktopRouterKey; dwm_force_cpu = $null }
if ($KmdDesktopRouterKey) {
 $routerKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($KmdDesktopRouterKey)
 if (!$routerKey) { throw 'Router key missing' }
 try {
  $result.desktop.dwm_force_cpu = $routerKey.GetValue('DwmForceCpu')
  if ($routerKey.GetValueKind('DwmForceCpu') -ne [Microsoft.Win32.RegistryValueKind]::DWord -or $result.desktop.dwm_force_cpu -ne 1) { throw 'DWM not held on the CPU route: DwmForceCpu must be 1 (route.py NNN cpu first)' }
 } finally { $routerKey.Dispose() }
}
if ($result.test_processes.Count) { throw 'Concurrent lab workload' }
if (@($result.running_tasks | Where-Object { $_.TaskName -notin @('BC250 monitor overlay','BC250 net watchdog',$KmdTaskName) }).Count) { throw ('Concurrent lab task: ' + (@($result.running_tasks | ForEach-Object TaskName) -join ', ')) }
if($parameters['UnconfirmedStarts'].value -ne 0){throw 'Baseline gate/guard changed: UnconfirmedStarts'}
foreach($name in 'EnableGpuPresentBlit','EnableCddDwmInterop') {
 if(!$parameters.Contains($name) -or $parameters[$name].kind -ne 'DWord' -or $parameters[$name].value -ne [int]$KmdDesktopSwitches){throw ('Baseline gate/guard changed: '+$name)}
}
# Switches 1: this start latched both on (interop.c InteropStart writes InteropLastState = effective | requested << 8).
if([int]$KmdDesktopSwitches -eq 1 -and (!$parameters.Contains('InteropLastState') -or $parameters['InteropLastState'].value -ne 0x303 -or $parameters['InteropLastReason'].value -ne 0)){throw 'Interop latch is not 0x303 reason 0'}
$result | ConvertTo-Json -Depth 8
