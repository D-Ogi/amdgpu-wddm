# C62 open-loop thermal identification (LAB, elevated SSH). The GPU clock is pinned by the start's ceiling
# (DpmMaxMHz 1000 = the lab floor, so the governor has one level; idle still drops to 500 MHz, a measured power
# step). The fan is fixed for the whole trial (lease). Power steps come from a fixed schedule of GPU and CPU loads:
#   0-20 s idle, 20-70 s GPU fill, 70-100 s GPU + 4 CPU threads, 100-125 s idle, 125-150 s 4 CPU threads only.
# One telemetry call per 2 s; the thermal stop (Tctl >= 87 C for 10 s or >= 89 C) ends the loads at once.
param([ValidateRange(20,100)][int]$FanPct = 100, [Parameter(Mandatory)][string]$Tag)
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$runner = 'C:\BC250\m14\app-route-001\ops\app-run.ps1'
$dir = "C:\BC250\tmp\c62\$Tag"
$null = New-Item -ItemType Directory -Force -Path $dir
try { if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { 'owner STOP'; exit 3 } } catch {}
"tag=$Tag fan=$FanPct start=$([DateTime]::UtcNow.ToString('o'))"
& $cli dpm 2>&1 | Select-Object -Last 1
& $cli fan set $FanPct 170 2>&1 | Select-Object -First 1
$tel = "$dir\telemetry.txt"; $null = New-Item -ItemType File -Force -Path $tel
$gpu = $null; $jobs = @(); $phase = ''; $stop = ''; $hot = $null
function CpuOn([int]$ms) { 1..4 | ForEach-Object { Start-Job -ArgumentList $ms -ScriptBlock { param($ms) $sw = [Diagnostics.Stopwatch]::StartNew(); $x = 0; while ($sw.ElapsedMilliseconds -lt $ms) { $x++ } } } }
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 150) {
    $t = $sw.Elapsed.TotalSeconds
    $want = if ($t -lt 20) { 'idle' } elseif ($t -lt 70) { 'gpu' } elseif ($t -lt 100) { 'gpu+cpu' } elseif ($t -lt 125) { 'idle2' } else { 'cpu' }
    if ($want -ne $phase) {
        $phase = $want
        if ($phase -eq 'gpu') { $gpu = Start-Process powershell.exe -PassThru -WindowStyle Hidden -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Client d3d11bench -Seconds 82 -Arguments "--mode offscreen --scenes fill --layers 32 --size 1920x1080 --frames 100000"' -f $runner) }
        if ($phase -eq 'gpu+cpu') { $jobs += CpuOn 30000 }
        if ($phase -eq 'idle2') { Get-Process d3d11bench -ErrorAction SilentlyContinue | Stop-Process -Force }
        if ($phase -eq 'cpu') { $jobs += CpuOn 25000 }
        ('phase {0} at t_ms={1}' -f $phase, $sw.ElapsedMilliseconds) | Add-Content $tel
    }
    Start-Sleep -Milliseconds 1700
    $s = @(& $cli telemetry 2>&1)
    ('t_ms=' + $sw.ElapsedMilliseconds + ' phase=' + $phase) | Add-Content $tel
    $s | Add-Content $tel
    $f = ($s | Where-Object { $_ -match '^fan ' }) -join ' '
    if ($f -match 'tsi_c=([\d.]+)') {
        $tc = [double]$Matches[1]
        if ($tc -ge 89) { $stop = "tctl $tc >= 89"; break }
        if ($tc -ge 87) { if (-not $hot) { $hot = $sw.Elapsed.TotalSeconds } elseif ($sw.Elapsed.TotalSeconds - $hot -ge 10) { $stop = 'tctl >= 87 for 10 s'; break } } else { $hot = $null }
    }
}
$jobs | Stop-Job -PassThru -ErrorAction SilentlyContinue | Remove-Job -Force -ErrorAction SilentlyContinue
Get-Process d3d11bench -ErrorAction SilentlyContinue | Stop-Process -Force
& $cli fan curve standard 2>&1 | Select-Object -First 1
"stop=$(if ($stop) { $stop } else { 'time' }) elapsed=$([int]$sw.Elapsed.TotalSeconds)"
$b = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
'WHEA/4101 since boot: {0}/{1}' -f @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-WHEA-Logger'; StartTime = $b } -ErrorAction SilentlyContinue).Count, @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 4101; StartTime = $b } -ErrorAction SilentlyContinue).Count
'--- telemetry'
Get-Content $tel
