param(
    [string]$Exe = '',
    [string]$Runs = '',
    [switch]$Warp
)
# The host check matrix for this development PC. Every run is short, in a small borderless window, and the
# process exits on its own; nothing is left resident. The point is the self-consistency of the numbers, not
# performance of this host: frame interval should be about max(CPU path, GPU busy + idle) and the calibration
# within 10 % of the asked GPU work.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\work-dir.ps1"
$work = Get-FrameloopWork
if (-not $Exe) { $Exe = Join-Path $work 'build\amdgpu_wddm_frameloop.exe' }
if (-not $Runs) { $Runs = Join-Path $work 'runs' }
New-Item -ItemType Directory -Force $Runs | Out-Null
$tag = if ($Warp) { 'warp' } else { 'gpu' }
$adapter = if ($Warp) { @('--warp') } else { @() }
# WARP is a software rasterizer: the same GPU work would take minutes, so its runs ask for far less and the
# window is smaller. Calibration still has to land on what was asked.
$seconds = if ($Warp) { 8 } else { 6 }
$size = if ($Warp) { '640x360' } else { '1280x720' }
$cases = @(
    @{ Name = 'gpums0-l1';     Args = @('--gpu-ms', '0',  '--lists', '1', '--latency', '2') },
    @{ Name = 'gpums0-l7';     Args = @('--gpu-ms', '0',  '--lists', '7', '--latency', '2') },
    @{ Name = 'gpums4-l7';     Args = @('--gpu-ms', '4',  '--lists', '7', '--latency', '2') },
    @{ Name = 'gpums12-l7';    Args = @('--gpu-ms', '12', '--lists', '7', '--latency', '2') },
    @{ Name = 'gpums12-l1';    Args = @('--gpu-ms', '12', '--lists', '1', '--latency', '2') },
    @{ Name = 'gpums12-lat1';  Args = @('--gpu-ms', '12', '--lists', '7', '--latency', '1') },
    @{ Name = 'gpums12-lat3';  Args = @('--gpu-ms', '12', '--lists', '7', '--latency', '3', '--buffers', '3') },
    @{ Name = 'gpums12-vsync'; Args = @('--gpu-ms', '12', '--lists', '7', '--latency', '2', '--present-interval', '1') },
    @{ Name = 'gpums12-wait';  Args = @('--gpu-ms', '12', '--lists', '7', '--latency', '2', '--frame-latency-waitable', '2') }
)
if ($Warp) {
    $cases = $cases | ForEach-Object {
        $a = @($_.Args)
        for ($i = 0; $i -lt $a.Count - 1; $i++) {
            if ($a[$i] -eq '--gpu-ms' -and $a[$i + 1] -eq '4')  { $a[$i + 1] = '2' }
            if ($a[$i] -eq '--gpu-ms' -and $a[$i + 1] -eq '12') { $a[$i + 1] = '6' }
        }
        @{ Name = $_.Name; Args = $a }
    }
}
$rows = @()
foreach ($case in $cases) {
    $out = Join-Path $Runs "host-$tag-$($case.Name).json"
    $argv = @('--seconds', $seconds, '--size', $size, '--out', $out) + $adapter + $case.Args
    Write-Host "== $tag $($case.Name): $($case.Args -join ' ')"
    & $Exe @argv | ForEach-Object { Write-Host "   $_" }
    $code = $LASTEXITCODE
    $json = Get-Content -Raw -LiteralPath $out | ConvertFrom-Json
    $rows += [pscustomobject]@{
        case = $case.Name
        exit = $code
        fps = $json.summary.fps
        asked_gpu_ms = $json.config.gpu_ms
        gpu_busy_p50 = $json.summary.gpu_ms.busy.p50
        busy_err_pct = $json.calibration.gpu_busy_error_pct
        interval_p50 = $json.summary.cpu_ms.frame_interval.p50
        idle_p50 = $json.summary.gpu_ms.idle_per_frame.p50
        idle_await_p50 = $json.summary.gpu_ms.idle_awaiting_submission_per_frame.p50
        idle_after_p50 = $json.summary.gpu_ms.idle_after_submission_per_frame.p50
        inter_gap_p50 = $json.summary.gpu_ms.inter_frame_gap.p50
        submit_p50 = $json.summary.latency_ms.submit_to_gpu_start.p50
        wake_p50 = $json.summary.latency_ms.gpu_end_to_wake.p50
        wake_p99 = $json.summary.latency_ms.gpu_end_to_wake.p99
        present_p50 = $json.summary.cpu_ms.present.p50
        status = $json.result.status
    }
}
$rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Runs "host-$tag-table.json") -Encoding ascii
