# BD-021: ask the running KMD for its log summary (BC250_ESCAPE_LOG_SUMMARY) and print the vidmm lines.
# Read only: the escape logs counters, it changes no state.
$cli = Get-ChildItem -Path 'C:\Program Files\BC250*', 'C:\Program Files\amdgpu-wddm*', 'C:\BC250' -Recurse -Filter 'bc250kmd_cli.exe' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $cli) { 'no bc250kmd_cli.exe found'; exit 1 }
"cli: $($cli.FullName)"
"utc: $([DateTime]::UtcNow.ToString('s'))Z"
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
"last boot: $($boot.ToString('s'))Z"
& $cli.FullName version 2>&1 | Select-Object -First 3
$out = & $cli.FullName log summary 2>&1
"summary lines: $($out.Count)"
$out | Where-Object { $_ -match 'vidmm summary|wddm summary: no TDR|TDR:|driver version|0\.7\.216\.' } | Select-Object -First 80
