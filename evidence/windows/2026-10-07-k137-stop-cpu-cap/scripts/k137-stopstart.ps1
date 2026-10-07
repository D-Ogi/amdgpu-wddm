# LAB K137 bisect (~2.5 min): one-thread CPU clock before, after a disable (the KMD stop alone) and after the enable
# (the KMD start), each 8 s. The desktop falls back to the basic display driver while the GPU is disabled.
param([switch]$StartOnly)
function Probe([string]$label) {
    $job = Start-Job { $e = (Get-Date).AddSeconds(9); $x = 0; while ((Get-Date) -lt $e) { $x++ } }
    Start-Sleep 2
    $s = (Get-Counter '\Processor Information(*)\% Processor Performance', '\Processor Information(*)\% Processor Time' -SampleInterval 1 -MaxSamples 5).CounterSamples
    $time = $s | Where-Object { $_.Path -like '*processor time' -and $_.InstanceName -notlike '*_total' }
    $busy = $time | Group-Object InstanceName | Sort-Object { ($_.Group | Measure-Object CookedValue -Average).Average } -Descending | Select-Object -First 1
    $p = ($s | Where-Object { $_.Path -like '*processor performance' -and $_.InstanceName -eq $busy.Name } | Measure-Object CookedValue -Average).Average
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    "$label $(Get-Date -Format HH:mm:ss) perf $([math]::Round($p,1)) % = $([int](3194 * $p / 100)) MHz"
}
$d = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
Probe 'before'
& pnputil.exe /disable-device "$($d.InstanceId)" 2>&1 | Select-Object -Last 1
Start-Sleep 5
"status $((Get-PnpDevice -InstanceId $d.InstanceId).Status)"
Probe 'disabled'
& pnputil.exe /enable-device "$($d.InstanceId)" 2>&1 | Select-Object -Last 1
for ($i = 0; $i -lt 20 -and (Get-PnpDevice -InstanceId $d.InstanceId).Status -ne 'OK'; $i++) { Start-Sleep 3 }
Start-Sleep 15
"status $((Get-PnpDevice -InstanceId $d.InstanceId).Status)"
Probe 'enabled'
