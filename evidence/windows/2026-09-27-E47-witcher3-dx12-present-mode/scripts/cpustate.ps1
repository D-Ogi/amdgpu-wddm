# Read-only CPU-side state: processor load and performance, top CPU consumers over 3 s, Defender scan status, power plan.
param([string]$Tag = 'snapshot')
$ErrorActionPreference = 'Continue'
$c1 = Get-Counter -Counter '\Processor(_Total)\% Processor Time', '\Processor Information(_Total)\% Processor Performance', '\Processor Information(_Total)\Processor Frequency', '\Processor Information(_Total)\% of Maximum Frequency', '\System\Processor Queue Length', '\Memory\Pages/sec' -SampleInterval 1 -MaxSamples 3 -ErrorAction SilentlyContinue
$ctr = @{}
if ($c1) { foreach ($set in $c1) { foreach ($s in $set.CounterSamples) { $k = ($s.Path -replace '^\\\\[^\\]+', ''); if (-not $ctr.ContainsKey($k)) { $ctr[$k] = @() }; $ctr[$k] += [math]::Round($s.CookedValue, 1) } } }
$a = Get-Process | ForEach-Object { @{ pid = $_.Id; name = $_.ProcessName; cpu = $_.TotalProcessorTime.TotalSeconds } }
Start-Sleep -Seconds 3
$b = @{}
Get-Process | ForEach-Object { $b[$_.Id] = $_.TotalProcessorTime.TotalSeconds }
$top = $a | ForEach-Object { if ($b.ContainsKey($_.pid)) { @{ name = $_.name; pid = $_.pid; cpu_s_per_3s = [math]::Round($b[$_.pid] - $_.cpu, 2) } } } | Where-Object { $_.cpu_s_per_3s -gt 0.05 } | Sort-Object { $_.cpu_s_per_3s } -Descending | Select-Object -First 10
$mp = $null
try { $m = Get-MpComputerStatus -ErrorAction Stop; $mp = @{ realtime = $m.RealTimeProtectionEnabled; quick_start = "$($m.QuickScanStartTime)"; quick_end = "$($m.QuickScanEndTime)"; full_start = "$($m.FullScanStartTime)"; full_end = "$($m.FullScanEndTime)"; scan_age_days = $m.QuickScanAge; signatures = "$($m.AntivirusSignatureLastUpdated)" } } catch { $mp = @{ error = $_.Exception.Message } }
$plan = (powercfg /getactivescheme 2>$null | Out-String).Trim()
$snap = [ordered]@{ tag = $Tag; utc = [DateTime]::UtcNow.ToString('o'); counters = $ctr; top_cpu_3s = @($top); defender = $mp; power_plan = $plan; uptime_h = [math]::Round(((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalHours, 1) }
$json = $snap | ConvertTo-Json -Depth 5
$json | Set-Content -LiteralPath "C:\BC250\m12\witcher3-dx12\cpustate-$Tag.json" -Encoding ASCII
$json
