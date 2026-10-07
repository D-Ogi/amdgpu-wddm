# DP audio, Windows side, read only (no writes, no device restarts, no playback): how the GPU's HD Audio
# function (PCI 1002:13FF) gets its interrupts and its memory window, and the same facts for the GPU itself.
# Answers: line-based (INTx) or MSI for HDAudBus, which IRQ, is that IRQ shared, the BAR0 range (for the
# KMD read-only peek), and the DMA-remapping and interrupt properties PnP reports. Runs in a few seconds.
# Run: python tools\win\target.py ps P:\BC-250\scratch\dp-audio\probe\hda-inventory.ps1
$ErrorActionPreference = 'Continue'
$hda = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FF*' } | Select-Object -First 1
$gpu = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
if (-not $hda) { 'no 1002:13FF function present'; exit 1 }
$all = @(Get-CimInstance Win32_PnPEntity)
$irqAlloc = @(Get-CimInstance Win32_PnPAllocatedResource -ErrorAction SilentlyContinue |
    Where-Object { $_.Antecedent.CimSystemProperties.ClassName -eq 'Win32_IRQResource' })
foreach ($d in @($hda, $gpu)) {
    if (-not $d) { continue }
    $id = $d.InstanceId
    '=== {0} [{1}] {2}' -f $d.FriendlyName, $d.Status, ($id -replace '\\[0-9A-F&]{8,}$', '\<inst>')
    $svc = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
    'service {0}' -f $svc
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties"
    if (Test-Path $k) {
        $p = Get-ItemProperty $k
        'registry MSISupported={0} MessageNumberLimit={1}' -f $p.MSISupported, $p.MessageNumberLimit
    } else { 'registry MSI key absent (driver default: line-based)' }
    Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue |
        Where-Object { $_.KeyName -match 'Interrupt|Msi|Dma|Remap|BusNumber|Address|LocationInfo' } |
        ForEach-Object { 'prop {0} = {1}' -f $_.KeyName, ($_.Data -join ',') }
    $ent = $all | Where-Object { $_.DeviceID -eq $id }
    $irqs = @($ent | Get-CimAssociatedInstance -ResultClassName Win32_IRQResource -ErrorAction SilentlyContinue)
    foreach ($q in $irqs) {
        # MSI vectors show as large numbers (4294967xxx); a line IRQ is a small GSI number.
        $sharers = @($irqAlloc | Where-Object { $_.Antecedent.IRQNumber -eq $q.IRQNumber -and $_.Dependent.DeviceID -ne $id } |
            ForEach-Object { $dep = $_.Dependent.DeviceID; ($all | Where-Object { $_.DeviceID -eq $dep } | Select-Object -First 1).Name })
        'irq {0} trigger={1} shareable={2} shared-with {3} other device(s): {4}' -f $q.IRQNumber, $q.TriggerType, $q.Shareable, $sharers.Count,
            ($sharers -join '; ')
    }
    if ($irqs.Count -eq 0) { 'irq: none reported' }
    $mem = @($ent | Get-CimAssociatedInstance -ResultClassName Win32_DeviceMemoryAddress -ErrorAction SilentlyContinue)
    foreach ($m in $mem) { 'mem 0x{0:X} - 0x{1:X} ({2} KB)' -f $m.StartingAddress, $m.EndingAddress, [int](($m.EndingAddress - $m.StartingAddress + 1) / 1024) }
}
'=== inbox INF interrupt line (lab copy)'
$inf = Join-Path $env:windir 'INF\hdaudbus.inf'
if (Test-Path $inf) { Select-String -Path $inf -Pattern 'MSISupported|DriverVer' | ForEach-Object { $_.Line.Trim() } }
'=== HDAudBus driver file'
(Get-Item (Join-Path $env:windir 'System32\drivers\hdaudbus.sys')).VersionInfo.FileVersion
'=== Kernel DMA protection / device guard (DMA remapping context)'
Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue |
    ForEach-Object { 'VirtualizationBasedSecurityStatus={0} AvailableSecurityProperties={1}' -f $_.VirtualizationBasedSecurityStatus, ($_.AvailableSecurityProperties -join ',') }
'=== recent System log entries from HDAudBus / hdaudio / Kernel-PnP for the audio device (last 2 h, count and ids only)'
$since = (Get-Date).AddHours(-2)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'HDAudBus|HdAudio|Kernel-PnP|WHEA|IOMMU|DmaRemap' } |
    Group-Object ProviderName, Id | ForEach-Object { '{0} x{1}' -f $_.Name, $_.Count }
