# Read-only: PCIe link properties that Windows itself reports for every present PCI device.
# Speed codes: 1 = 2.5 GT/s, 2 = 5.0 GT/s, 3 = 8.0 GT/s, 4 = 16.0 GT/s.
$bios = Get-CimInstance Win32_BIOS
'BIOS {0} {1} {2}' -f $bios.Manufacturer, $bios.SMBIOSBIOSVersion, $bios.ReleaseDate.ToString('yyyy-MM-dd')
'OS build {0}' -f [Environment]::OSVersion.Version
$keys = @('DEVPKEY_PciDevice_CurrentLinkSpeed', 'DEVPKEY_PciDevice_CurrentLinkWidth',
          'DEVPKEY_PciDevice_MaxLinkSpeed', 'DEVPKEY_PciDevice_MaxLinkWidth',
          'DEVPKEY_Device_LocationInfo')
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\*' } | ForEach-Object {
    $id = $_.InstanceId
    $p = @{}
    foreach ($k in $keys) {
        try { $p[$k] = (Get-PnpDeviceProperty -InstanceId $id -KeyName $k -ErrorAction Stop).Data } catch { $p[$k] = '' }
    }
    $vd = ($id -split '\\')[1] -replace '&SUBSYS.*$', ''
    '{0,-22} {1,-38} cur={2}/x{3} max={4}/x{5} {6}' -f $vd, $_.FriendlyName,
        $p['DEVPKEY_PciDevice_CurrentLinkSpeed'], $p['DEVPKEY_PciDevice_CurrentLinkWidth'],
        $p['DEVPKEY_PciDevice_MaxLinkSpeed'], $p['DEVPKEY_PciDevice_MaxLinkWidth'],
        $p['DEVPKEY_Device_LocationInfo']
}
