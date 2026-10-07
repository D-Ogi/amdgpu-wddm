# Which driver owns the GPU's HD Audio PCI function (the Azalia controller) and the DP audio endpoint, and with what
# settings: the controller service, its INF, hardware IDs, and the hdaudbus/HdAudio parameters present.
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002*' -or $_.InstanceId -like 'HDAUDIO\FUNC_01&VEN_1002*' } | ForEach-Object {
    $id = $_.InstanceId
    '--- {0} [{1}] {2}' -f $_.FriendlyName, $_.Status, ($id -replace '\\[^\\]+$', '\...')
    foreach ($k in 'DEVPKEY_Device_Service', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_DriverProvider', 'DEVPKEY_Device_HardwareIds') {
        '  {0} = {1}' -f ($k -replace 'DEVPKEY_Device_', ''), ((Get-PnpDeviceProperty -InstanceId $id -KeyName $k -ErrorAction SilentlyContinue).Data -join ' ')
    }
}
foreach ($s in 'HDAudBus', 'HdAudAddService') {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Services\$s"
    if (Test-Path $k) { "--- service $s"; Get-ChildItem $k -Recurse -ErrorAction SilentlyContinue | ForEach-Object { '  ' + $_.Name; (Get-ItemProperty $_.PSPath).PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { '    {0} = {1}' -f $_.Name, $_.Value } } }
}
