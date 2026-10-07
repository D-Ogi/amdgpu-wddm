# LAB read-only (8 s of one busy thread): effective clock of one busy thread pinned to logical processor 2, the K137
# check of run-m157.sh (2026-10-07: a KMD stop leaves the CPU at ~2.72-2.80 GHz until a Windows restart).
$job = Start-Job { $e = (Get-Date).AddSeconds(9); $x = 0; while ((Get-Date) -lt $e) { $x++ } }
Start-Sleep -Milliseconds 700
$child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$PID" | Where-Object { $_.Name -like 'powershell*' } | Select-Object -First 1
if ($child) { (Get-Process -Id $child.ProcessId).ProcessorAffinity = [IntPtr]4 }
Start-Sleep 1
$s = (Get-Counter '\Processor Information(0,2)\% Processor Performance', '\Processor Information(0,2)\% Processor Time' -SampleInterval 1 -MaxSamples 5).CounterSamples
$p = ($s | Where-Object { $_.Path -like '*processor performance' } | Measure-Object CookedValue -Average).Average
$t = ($s | Where-Object { $_.Path -like '*processor time' } | Measure-Object CookedValue -Average).Average
"busiest 0,2 pinned $([bool]$child) time $([math]::Round($t,1)) % perf $([math]::Round($p,1)) % = $([int](3194 * $p / 100)) MHz"
Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue
