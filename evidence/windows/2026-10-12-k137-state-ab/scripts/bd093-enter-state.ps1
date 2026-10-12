# BD-093 / K137: enter the K137 state the cheap way. One PnP restart of the GPU device, no Windows restart.
# This is the only step of the kit that changes machine state, and it is the defect's own trigger
# (DEFECTS.md BD-093: the state appears after any stop of the GPU device with the native SMU owner on).
# It writes nothing to the hardware itself.
#
#   powershell -ExecutionPolicy Bypass -File bd093-enter-state.ps1
param([int]$WaitSeconds = 60, [string]$Work = 'C:\BC250\tmp\bd093')

$ErrorActionPreference = 'Continue'
$log = Join-Path $Work 'enter-state.txt'
if (-not (Test-Path $Work)) { New-Item -ItemType Directory -Path $Work -Force | Out-Null }

function Say([string]$text) {
    $line = (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff') + ' enter ' + $text
    Write-Output $line
    Add-Content -Path $log -Value $line -Encoding Ascii
}

$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
if ($null -eq $dev) { Say 'FATAL no PCI\VEN_1002&DEV_13FE device'; exit 2 }
Say ('device before status=' + $dev.Status + ' instance=' + $dev.InstanceId)

$temp = & $cli 'dpm' 2>&1 | Select-Object -First 2
foreach ($l in $temp) { Say ('dpm before ' + ([string]$l).Trim()) }

$out = & pnputil.exe /restart-device $dev.InstanceId 2>&1
foreach ($l in $out) { Say ('pnputil ' + ([string]$l).Trim()) }

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$status = ''
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    $status = (Get-PnpDevice -InstanceId $dev.InstanceId).Status
    if ($status -eq 'OK') { break }
}
Say ('device after status=' + $status)
if ($status -ne 'OK') { Say 'WARNING the device did not come back OK: do not measure, recover first'; exit 3 }

$after = & $cli 'dpm' 2>&1 | Select-Object -First 2
foreach ($l in $after) { Say ('dpm after ' + ([string]$l).Trim()) }
$h = & $cli 'health' 2>&1 | Select-Object -First 6
foreach ($l in $h) { Say ('health ' + ([string]$l).Trim()) }
Say 'END the device has been stopped and started once in this boot'
