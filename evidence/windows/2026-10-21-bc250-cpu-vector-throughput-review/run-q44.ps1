# The lab run of this directory (review questions 13-35). Refuses to run when a game, a GPU trial, vkcube,
# q2rtx or PresentMon is running, and prints the process names it saw (names only).
param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('o')) (lab clock; about 48 s fast per STATE.md 2026-10-21)"
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
"cpu $($cpu.Name) cores $($cpu.NumberOfCores) logical $($cpu.NumberOfLogicalProcessors) max $($cpu.MaxClockSpeed) MHz current $($cpu.CurrentClockSpeed) MHz"
$procs = Get-Process -ErrorAction SilentlyContinue
"processes ($($procs.Count)): $((($procs.ProcessName | Sort-Object -Unique) -join ','))"
$busy = $procs | Where-Object { $_.ProcessName -match 'q2rtx|vkcube|PresentMon|witcher3|ROTTR|llama|cs2|Factorio|3DMark|steam_?game|run-slot|bc250trial|vkmark|glmark' }
if ($busy) { "BUSY: $((($busy.ProcessName | Sort-Object -Unique) -join ','))"; "not run"; exit 3 }
$top = $procs | Where-Object { $_.CPU -gt 0 } | Sort-Object CPU -Descending | Select-Object -First 8
"top cpu-seconds: $(($top | ForEach-Object { '{0}={1:N0}' -f $_.ProcessName, $_.CPU }) -join ' ')"
$load0 = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
"cpu busy before: $load0 %"
$disk0 = (Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk -Filter "Name='_Total'").PercentDiskTime
"disk busy before: $disk0 %"
foreach ($run in @(
        @{ Exe = 'q44bench-clang.exe'; Args = @() },
        @{ Exe = 'q44bench-msvc.exe'; Args = @() },
        @{ Exe = 'cpubench-clang-E663.exe'; Args = @('15') },
        @{ Exe = 'cpubench-clang-E663.exe'; Args = @('15') },
        @{ Exe = 'cpubench-clang-E663.exe'; Args = @('15') })) {
    $exe = Join-Path $Dir $run.Exe
    "=== $($run.Exe) $($run.Args -join ' ') sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash) utc $([DateTime]::UtcNow.ToString('o'))"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $exe @($run.Args)
    "exit $LASTEXITCODE elapsed_s $([math]::Round($sw.Elapsed.TotalSeconds, 2))"
}
$load1 = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
"cpu busy after: $load1 %"
$disk1 = (Get-CimInstance Win32_PerfFormattedData_PerfDisk_PhysicalDisk -Filter "Name='_Total'").PercentDiskTime
"disk busy after: $disk1 %"
"utc end $([DateTime]::UtcNow.ToString('o'))"
