# LAB (~3 min): does the overlay cause or keep the K137 CPU state? Probe (one busy thread pinned to CPU 2) with the
# overlay running, after a GPU device restart, with the overlay stopped, and after it is started again.
function Probe([string]$label) {
    $job = Start-Job { $e = (Get-Date).AddSeconds(9); $x = 0; while ((Get-Date) -lt $e) { $x++ } }
    Start-Sleep -Milliseconds 700
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$PID" | Where-Object { $_.Name -like 'powershell*' } | Select-Object -First 1
    if ($child) { (Get-Process -Id $child.ProcessId).ProcessorAffinity = [IntPtr]4 }
    Start-Sleep 1
    $s = (Get-Counter '\Processor Information(0,2)\% Processor Performance' -SampleInterval 1 -MaxSamples 5).CounterSamples
    $p = ($s | Measure-Object CookedValue -Average).Average
    Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue
    $ov = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'bc250mon*' }).Count
    "$label $(Get-Date -Format HH:mm:ss) overlay processes $ov perf $([math]::Round($p,1)) % = $([int](3194 * $p / 100)) MHz"
}
$task = 'BC250 monitor overlay'
Probe 'fresh-boot, overlay on'
$d = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
& pnputil.exe /restart-device "$($d.InstanceId)" 2>&1 | Select-Object -Last 1
Start-Sleep 25
"device $((Get-PnpDevice -InstanceId $d.InstanceId).Status)"
Probe 'after device restart, overlay on'
Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'bc250mon*' } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 5
Probe 'overlay stopped'
Start-Sleep 10
Probe 'overlay stopped, 15 s later'
Start-ScheduledTask -TaskName $task
Start-Sleep 10
Probe 'overlay started again'
