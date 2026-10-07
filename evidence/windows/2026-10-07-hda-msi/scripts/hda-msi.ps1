# E3: switch the GPU's HD Audio function (PCI 1002:13FF, inbox HDAudBus) between line-based interrupts (the
# inbox hdaudbus.inf default, MSISupported=0) and MSI (what Linux snd_hda_intel uses on this function), then
# restart only that device. Nothing else changes: no driver files, no reboot, no other device.
#   -Status : print the current value and the IRQ resource (read only)
#   -On     : back up the current value, write MSISupported=1, restart the device, print the new IRQ
#   -Off    : restore the backed-up value (or delete it if it was absent), restart the device
# A restart of the HDA controller drops the audio endpoints for a few seconds; audiosrv rebuilds them.
# Bounded: each pnputil call is given 60 s.
param([switch]$On, [switch]$Off, [switch]$Status)
$ErrorActionPreference = 'Continue'
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FF*' } | Select-Object -First 1
if (-not $dev) { 'no 1002:13FF function present'; exit 1 }
$id = $dev.InstanceId
$k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
$bak = 'C:\BC250\tmp\dpaudio-probe\msi-backup.txt'
function Show {
    $v = if (Test-Path $k) { (Get-ItemProperty $k -ErrorAction SilentlyContinue).MSISupported } else { 'absent' }
    $irq = Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -eq $id } |
        Get-CimAssociatedInstance -ResultClassName Win32_IRQResource -ErrorAction SilentlyContinue
    'status={0} MSISupported={1} irq={2}' -f (Get-PnpDevice -InstanceId $id).Status, $v, (($irq | ForEach-Object { $_.IRQNumber }) -join ',')
}
function Restart-Hda {
    $p = Start-Process -FilePath pnputil.exe -ArgumentList @('/restart-device', "`"$id`"") -NoNewWindow -PassThru -Wait:$false
    if (-not $p.WaitForExit(60000)) { $p.Kill(); 'pnputil did not finish in 60 s' } else { 'pnputil exit {0}' -f $p.ExitCode }
    Start-Sleep -Seconds 3
}
if ($Status -or (-not $On -and -not $Off)) { Show; exit 0 }
$null = New-Item -ItemType Directory -Force -Path (Split-Path $bak)
if ($On) {
    $old = if (Test-Path $k) { $p = Get-ItemProperty $k -ErrorAction SilentlyContinue; if ($null -ne $p.MSISupported) { "$($p.MSISupported)" } else { 'absent' } } else { 'nokey' }
    if (-not (Test-Path $bak)) { Set-Content -Path $bak -Value $old }
    'before: ' + (Show)
    if (-not (Test-Path $k)) { $null = New-Item -Path $k -Force }
    Set-ItemProperty -Path $k -Name MSISupported -Type DWord -Value 1
    Restart-Hda
    'after:  ' + (Show)
}
if ($Off) {
    $old = if (Test-Path $bak) { (Get-Content $bak).Trim() } else { '0' }
    if ($old -eq 'absent' -or $old -eq 'nokey') { Remove-ItemProperty -Path $k -Name MSISupported -ErrorAction SilentlyContinue }
    else { Set-ItemProperty -Path $k -Name MSISupported -Type DWord -Value ([int]$old) }
    Restart-Hda
    Remove-Item $bak -ErrorAction SilentlyContinue
    'restored ' + $old + ': ' + (Show)
}
