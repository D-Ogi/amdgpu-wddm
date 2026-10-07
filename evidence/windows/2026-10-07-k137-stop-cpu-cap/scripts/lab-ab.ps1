# LAB K137 A/B (~2.5 min) on KMD 0.7.216.15: SMU clocks and one pinned thread, fresh, then after a GPU device restart.
# -NoRestart: only the measurement (for a state already reached).
param([switch]$NoRestart, [string]$Label = '')
$cli = 'C:\BC250\tmp\k137\bc250kmd_cli.exe'
function Probe([string]$label) {
    $job = Start-Job { $e = (Get-Date).AddSeconds(9); $x = 0; while ((Get-Date) -lt $e) { $x++ } }
    Start-Sleep -Milliseconds 700
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$PID" | Where-Object { $_.Name -like 'powershell*' } | Select-Object -First 1
    if ($child) { (Get-Process -Id $child.ProcessId).ProcessorAffinity = [IntPtr]4 }
    Start-Sleep 1
    $s = (Get-Counter '\Processor Information(0,2)\% Processor Performance' -SampleInterval 1 -MaxSamples 3).CounterSamples
    $p = ($s | Measure-Object CookedValue -Average).Average
    "$label $(Get-Date -Format HH:mm:ss) one thread perf $([math]::Round($p,1)) % = $([int](3194 * $p / 100)) MHz"
    "  under load:"; & $cli dpm 2>&1 | Select-String 'smu clocks|smu metrics' | ForEach-Object { '    ' + $_.Line.Trim() }
    Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue
    Start-Sleep 3
    "  idle:"; & $cli dpm 2>&1 | ForEach-Object { '    ' + "$_".Trim() }
}
& $cli log 2>&1 | Select-String "baseline|smu metrics: .*MHz soc" | Select-Object -First 4 | ForEach-Object { '  ' + $_.Line.Trim() }
Probe "$Label state A"
if (-not $NoRestart) {
    $d = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
    & pnputil.exe /restart-device "$($d.InstanceId)" 2>&1 | Select-Object -Last 1
    Start-Sleep 30
    "device $((Get-PnpDevice -InstanceId $d.InstanceId).Status) $((Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data)"
    Probe "$Label after device restart"
    & $cli log 2>&1 | Select-String "baseline" | Select-Object -First 2 | ForEach-Object { '  ' + $_.Line.Trim() }
}
