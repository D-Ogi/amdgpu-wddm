# C62 grid sweep, one trial (LAB, elevated SSH). A fixed load (d3d11bench offscreen fill, 1920x1080, 32 layers, plus
# $CpuThreads busy threads), one setting of the three actuators (fan duty, GPU V/F offset, CPU undervolt), samples to
# CSV. One telemetry call every 2 s (about one CLI process per 2 s, not the per-second loop of BD-051). CPU rail and the V/F "now" line
# every 10 s. Thermal stop: Tctl >= 87 C for 10 s or >= 89 C at once. Every trial ends with cancel and the fan back
# to its durable setting. At most $Seconds (<= 160) plus about 10 s of set-up and clean-up.
param([int]$FanPct = 0, [int]$GpuMv = 0, [int]$CpuUv = 0, [ValidateRange(30,160)][int]$Seconds = 150,
      [ValidateRange(0,10)][int]$CpuThreads = 2, [Parameter(Mandatory)][string]$Tag)
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$runner = 'C:\BC250\m14\app-route-001\ops\app-run.ps1'
$dir = "C:\BC250\tmp\c62\$Tag"
$null = New-Item -ItemType Directory -Force -Path $dir
$t0 = [DateTime]::UtcNow
"tag=$Tag fan=$FanPct gpu_mv=$GpuMv cpu_uv=$CpuUv seconds=$Seconds threads=$CpuThreads start=$($t0.ToString('o'))"
try { if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { 'owner STOP'; exit 3 } } catch {}

$win = ($Seconds + 15) * 1000
if ($FanPct -gt 0) { & $cli fan set $FanPct ([Math]::Min(300, $Seconds + 15)) 2>&1 | Select-Object -First 1 }
if ($GpuMv -gt 0) { & $cli dpm curve offset $GpuMv $win 2>&1 | Select-Object -First 1 }
if ($CpuUv -gt 0) { & $cli cpu set uv $CpuUv window $win 2>&1 | Select-Object -First 1 }

$load = Start-Process powershell.exe -PassThru -WindowStyle Hidden -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Client d3d11bench -Seconds {1} -Arguments "--mode offscreen --scenes fill --layers 32 --size 1920x1080 --frames 100000"' -f $runner, ($Seconds + 5))
$jobs = @()
if ($CpuThreads -gt 0) {
    $ms = $Seconds * 1000
    $jobs = 1..$CpuThreads | ForEach-Object { Start-Job -ArgumentList $ms -ScriptBlock { param($ms) $sw = [Diagnostics.Stopwatch]::StartNew(); $x = 0; while ($sw.ElapsedMilliseconds -lt $ms) { $x++ } } }
}
# One telemetry call per 2 s: a long "telemetry N" process writes into a redirected file only at exit (C stdio
# buffering), so the thermal stop could not read it (smoke trial 03:05Z, Tctl reached 90 C unseen).
$telFile = "$dir\telemetry.txt"
$null = New-Item -ItemType File -Force -Path $telFile
$slow = "$dir\slow.csv"
'utc,cpu_mv,cores_mhz,cpu_c,gpu_now' | Set-Content $slow
$hot = $null; $stop = ''
$sw = [Diagnostics.Stopwatch]::StartNew(); $next = 0
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 1700
    $sample = @(& $cli telemetry 2>&1)
    ('t_ms=' + $sw.ElapsedMilliseconds + ' utc=' + [DateTime]::UtcNow.ToString('o')) | Add-Content $telFile
    $sample | Add-Content $telFile
    $last = ($sample | Where-Object { $_ -match '^fan ' }) -join ' '
    if ($last -match 'tsi_c=([\d.]+)') {
        $tc = [double]$Matches[1]
        if ($tc -ge 89) { $stop = "tctl $tc >= 89"; break }
        if ($tc -ge 87) { if (-not $hot) { $hot = $sw.Elapsed.TotalSeconds } elseif ($sw.Elapsed.TotalSeconds - $hot -ge 10) { $stop = "tctl >= 87 for 10 s"; break } } else { $hot = $null }
    }
    if ($sw.Elapsed.TotalSeconds -ge $next) {
        $next += 10
        $null = & $cli cpu readback 2>&1
        $c = (& $cli cpu 2>&1) -join "`n"
        $mv = if ($c -match 'cpu: (\d+) mV') { $Matches[1] } else { '' }
        $cc = if ($c -match 'features 0x[0-9A-F]+, ([\d.]+) C') { $Matches[1] } else { '' }
        $cores = if ($c -match 'cores ([\d ]+) MHz') { $Matches[1].Trim() } else { '' }
        $now = (& $cli dpm curve 2>&1 | Where-Object { $_ -match 'dpm curve: now' }) -replace '^dpm curve: now ', '' -replace ',', ';'
        '{0},{1},{2},{3},{4}' -f [DateTime]::UtcNow.ToString('o'), $mv, $cores, $cc, $now | Add-Content $slow
    }
}
$jobs | Stop-Job -PassThru -ErrorAction SilentlyContinue | Remove-Job -Force -ErrorAction SilentlyContinue
if (-not $load.HasExited) { $load.WaitForExit(15000) | Out-Null }
if (-not $load.HasExited) { Get-Process d3d11bench -ErrorAction SilentlyContinue | Stop-Process -Force; $load.Kill() }
if ($CpuUv -gt 0) { & $cli cpu cancel 2>&1 | Select-Object -First 1 }
if ($GpuMv -gt 0) { & $cli dpm curve cancel 2>&1 | Select-Object -First 1 }
& $cli fan curve standard 2>&1 | Select-Object -First 1
"stop=$(if ($stop) { $stop } else { 'time' }) elapsed=$([int]$sw.Elapsed.TotalSeconds)"
$b = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
'WHEA since boot: ' + @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-WHEA-Logger'; StartTime = $b } -ErrorAction SilentlyContinue).Count
'4101 since boot: ' + @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 4101; StartTime = $b } -ErrorAction SilentlyContinue).Count
& $cli log 2>&1 | Select-String -Pattern 'GPU FAULT|FENCE TIMEOUT' | Select-Object -Last 3 | ForEach-Object { $_.Line.Trim() }
'--- telemetry'
Get-Content $telFile
'--- slow'
Get-Content $slow
