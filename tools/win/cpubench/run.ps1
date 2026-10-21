# Runs the three cpubench builds in -Dir one after the other and prints the context of the run before them: UTC,
# the CPU as Windows names it, and the processes of a GPU or game trial that run at the same time (the bench pins one
# logical CPU, so a trial beside it disturbs both). Copy it to unit A with the builds and run it with
# `python tools/win/target.py ps tools/win/cpubench/run.ps1 -Dir <directory on the target>`.
param(
    [Parameter(Mandatory)][string]$Dir,
    [int]$Repeats = 15
)
"utc $([DateTime]::UtcNow.ToString('o'))"
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
"cpu $($cpu.Name) cores $($cpu.NumberOfCores) logical $($cpu.NumberOfLogicalProcessors) max $($cpu.MaxClockSpeed) MHz"
$busy = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.CPU -gt 0 -and $_.ProcessName -match 'q2rtx|vkcube|PresentMon|witcher3|ROTTR|llama' }
if ($busy) { "note: running at the same time: $(($busy.ProcessName | Sort-Object -Unique) -join ',')" }
foreach ($name in 'cpubench.exe', 'cpubench-avx2.exe', 'cpubench-clang.exe') {
    $exe = Join-Path $Dir $name
    "=== $name sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash)"
    & $exe $Repeats
    "exit $LASTEXITCODE"
}
