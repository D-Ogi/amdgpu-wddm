param(
    [string]$Exe = '',
    [string]$Runs = '',
    [switch]$Warp
)
# Multithreaded recording and batched submission (owner rule: test multithreading with our own clients first).
# Three questions, one matrix:
#   interleaved vs sequential at one thread  - what moving the submit out of the record loop costs by itself
#   sequential at 1 / 4 / 8 threads          - what parallel recording buys on the frame's wall clock
#   sequential vs batch                      - what one ExecuteCommandLists for all K lists buys
# 7 lists is the game-shaped case; 32 lists is where recording is actually worth parallelising. Each run is
# short, in a small borderless window, and exits on its own.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\work-dir.ps1"
$work = Get-FrameloopWork
if (-not $Exe) { $Exe = Join-Path $work 'build\amdgpu_wddm_frameloop.exe' }
if (-not $Runs) { $Runs = Join-Path $work 'runs' }
New-Item -ItemType Directory -Force $Runs | Out-Null
$tag = if ($Warp) { 'warp' } else { 'gpu' }
$adapter = if ($Warp) { @('--warp') } else { @() }
$seconds = 8
$size = if ($Warp) { '640x360' } else { '1280x720' }
# WARP's floor is about 0.28 ms per dispatch, so 32 lists cannot be asked for 6 ms of work; the 32-list rows
# there ask for 20 ms instead and the comparison stays within each list count.
$small = if ($Warp) { '6' } else { '12' }
$large = if ($Warp) { '20' } else { '12' }

$cases = @(
    @{ Name = 'l7-t1-interleaved';  Lists = 7;  Gpu = $small; Args = @('--record-threads', '1', '--submit', 'interleaved') },
    @{ Name = 'l7-t1-sequential';   Lists = 7;  Gpu = $small; Args = @('--record-threads', '1', '--submit', 'sequential') },
    @{ Name = 'l7-t1-batch';        Lists = 7;  Gpu = $small; Args = @('--record-threads', '1', '--submit', 'batch') },
    @{ Name = 'l7-t4-sequential';   Lists = 7;  Gpu = $small; Args = @('--record-threads', '4', '--submit', 'sequential') },
    @{ Name = 'l7-t4-batch';        Lists = 7;  Gpu = $small; Args = @('--record-threads', '4', '--submit', 'batch') },
    @{ Name = 'l32-t1-interleaved'; Lists = 32; Gpu = $large; Args = @('--record-threads', '1', '--submit', 'interleaved') },
    @{ Name = 'l32-t1-sequential';  Lists = 32; Gpu = $large; Args = @('--record-threads', '1', '--submit', 'sequential') },
    @{ Name = 'l32-t1-batch';       Lists = 32; Gpu = $large; Args = @('--record-threads', '1', '--submit', 'batch') },
    @{ Name = 'l32-t4-sequential';  Lists = 32; Gpu = $large; Args = @('--record-threads', '4', '--submit', 'sequential') },
    @{ Name = 'l32-t4-batch';       Lists = 32; Gpu = $large; Args = @('--record-threads', '4', '--submit', 'batch') },
    @{ Name = 'l32-t8-sequential';  Lists = 32; Gpu = $large; Args = @('--record-threads', '8', '--submit', 'sequential') },
    @{ Name = 'l32-t8-batch';       Lists = 32; Gpu = $large; Args = @('--record-threads', '8', '--submit', 'batch') }
)
$rows = @()
foreach ($case in $cases) {
    $out = Join-Path $Runs "mt-$tag-$($case.Name).json"
    $argv = @('--seconds', $seconds, '--size', $size, '--gpu-ms', $case.Gpu, '--lists', $case.Lists,
              '--latency', '2', '--present-interval', '0', '--out', $out) + $adapter + $case.Args
    Write-Host "== $tag $($case.Name)"
    & $Exe @argv | ForEach-Object { if ($_ -match 'FRAMELOOP|frames in|recording thread') { Write-Host "   $_" } }
    $code = $LASTEXITCODE
    $json = Get-Content -Raw -LiteralPath $out | ConvertFrom-Json
    $rows += [pscustomobject]@{
        case = $case.Name
        exit = $code
        threads = $json.config.effective_record_threads
        submit = $json.config.submit
        fps = $json.summary.fps
        rec_wall_p50 = $json.summary.cpu_ms.record.p50
        rec_wall_p99 = $json.summary.cpu_ms.record.p99
        rec_cpu_p50 = $json.summary.cpu_ms.record_cpu_total.p50
        exec_p50 = $json.summary.cpu_ms.execute_total.p50
        gpu_busy_p50 = $json.summary.gpu_ms.busy.p50
        busy_err = $json.calibration.gpu_busy_error_pct
        idle_p50 = $json.summary.gpu_ms.idle_per_frame.p50
        idle_await_p50 = $json.summary.gpu_ms.idle_awaiting_submission_per_frame.p50
        idle_after_p50 = $json.summary.gpu_ms.idle_after_submission_per_frame.p50
        interval_p50 = $json.summary.cpu_ms.frame_interval.p50
        status = $json.result.status
    }
}
$rows | Format-Table -AutoSize | Out-String -Width 230 | Write-Host
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Runs "mt-$tag-table.json") -Encoding ascii
