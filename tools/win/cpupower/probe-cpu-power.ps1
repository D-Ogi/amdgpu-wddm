# Read-only CPU power probe for unit A. It changes nothing: it queries the active power
# scheme's processor settings and samples two processor performance counters while idle.
#
# No setting is written. `powercfg /setacvalueindex`, `/setactive` and `/energy` are not used
# (`/energy` runs for 60 s or more and writes a report file).
#
#   python tools/win/target.py ps tools/win/cpupower/probe-cpu-power.ps1 [seconds]
#
# Output: one text block. The caller keeps it as evidence.

param([int]$Seconds = 20)

$ErrorActionPreference = "Stop"
Write-Output "probe-cpu-power 1.0"
Write-Output ("utc " + (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))
Write-Output ("host " + $env:COMPUTERNAME)
Write-Output ("os " + (Get-CimInstance Win32_OperatingSystem).Version)

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Write-Output ("cpu " + $cpu.Name.Trim())
Write-Output ("cores " + $cpu.NumberOfCores + " logical " + $cpu.NumberOfLogicalProcessors)
Write-Output ("max-clock-mhz " + $cpu.MaxClockSpeed)
Write-Output ("current-clock-mhz " + $cpu.CurrentClockSpeed)

Write-Output "--- active scheme"
$active = powercfg /getactivescheme
Write-Output $active
$guid = ([regex]::Match($active, "([0-9a-fA-F-]{36})")).Groups[1].Value

Write-Output "--- processor settings of the active scheme (powercfg /qh <scheme> SUB_PROCESSOR)"
# SUB_PROCESSOR is the processor power management subgroup. Query only, no index is set.
powercfg /qh $guid SUB_PROCESSOR

Write-Output "--- idle counter samples"
Write-Output ("samples " + $Seconds + " at 1 s")
$paths = @(
    "\Processor Information(_Total)\% of Maximum Frequency",
    "\Processor Information(_Total)\% Processor Performance",
    "\Processor Information(_Total)\% Processor Time",
    "\Processor Information(_Total)\Processor Frequency")
$data = Get-Counter -Counter $paths -SampleInterval 1 -MaxSamples $Seconds
$rows = @{}
foreach ($set in $data) {
    foreach ($s in $set.CounterSamples) {
        $name = $s.Path.Substring($s.Path.IndexOf(")\") + 2)
        if (-not $rows.ContainsKey($name)) { $rows[$name] = @() }
        $rows[$name] += [double]$s.CookedValue
    }
}
foreach ($name in $rows.Keys | Sort-Object) {
    $v = $rows[$name]
    $mean = ($v | Measure-Object -Average).Average
    $min = ($v | Measure-Object -Minimum).Minimum
    $max = ($v | Measure-Object -Maximum).Maximum
    Write-Output ("{0}: n {1} mean {2:N2} min {3:N2} max {4:N2}" -f $name, $v.Count, $mean, $min, $max)
}
Write-Output "probe-cpu-power end"
