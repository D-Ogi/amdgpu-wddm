param(
    [string]$Exe = '',
    [string]$Runs = '',
    [int]$Seconds = 5,
    [string]$Size = '1280x720'
)
# The host matrix of the cross-queue arms (--queues, --handoff, --signal-per-list), on this development PC.
# Every run is short, in a small borderless window, and the process exits on its own; nothing stays resident.
#
# This is not a measurement of the gap class: this host has a different scheduler state and a different GPU.
# It answers the two questions a lab trial cannot be run without: does each arm present correct frames through
# the second queue, and does each arm witness itself (fence_signals and fence_waits of the queue_handoff
# block). An arm whose fence never moved is void, exactly as an ICD arm with an unmoved counter is.
#
# It also reads the third witness of a two-queue arm: the clock. The second queue is asked for its timestamp
# frequency and for a GetClockCalibration pair, and the client refuses the arm unless the two queues report one
# frequency and their calibrations agree inside 1 ms. Without that, a crossing's duration carries an unknown
# offset and nothing timed across the queues can be read; with it, 'one domain' is in the trial's own JSON.
$ErrorActionPreference = 'Stop'
# The client and the run records live in the workspace, never in this repository.
. "$PSScriptRoot\work-dir.ps1"
$work = Get-FrameloopWork
if (-not $Exe) { $Exe = Join-Path $work 'build\amdgpu_wddm_frameloop.exe' }
if (-not $Runs) { $Runs = Join-Path $work 'runs' }
New-Item -ItemType Directory -Force $Runs | Out-Null
$cases = @(
    @{ Name = 'a-base';            Args = @();                                              Signals = 'zero'; Waits = 'zero'; Clock = 'one queue' },
    @{ Name = 'b-signal-per-list'; Args = @('--signal-per-list');                            Signals = 'many'; Waits = 'zero'; Clock = 'one queue' },
    @{ Name = 'c-2q-none';         Args = @('--queues', '2');                                Signals = 'zero'; Waits = 'zero'; Clock = 'one domain' },
    @{ Name = 'd-2q-per-list';     Args = @('--queues', '2', '--handoff', 'per-list');       Signals = 'many'; Waits = 'many'; Clock = 'one domain' },
    @{ Name = 'e-2q-per-frame';    Args = @('--queues', '2', '--handoff', 'per-frame');      Signals = 'many'; Waits = 'many'; Clock = 'one domain' }
)
$rows = @()
$failures = 0
foreach ($case in $cases) {
    $out = Join-Path $Runs "host-gpu-c48-$($case.Name).json"
    $argv = @('--seconds', $Seconds, '--size', $Size, '--out', $out, '--gpu-ms', '4', '--lists', '7',
              '--latency', '2', '--frame-statistics') + $case.Args
    Write-Host "== c48 $($case.Name): $($case.Args -join ' ')"
    & $Exe @argv | ForEach-Object { Write-Host "   $_" }
    $code = $LASTEXITCODE
    $json = Get-Content -Raw -LiteralPath $out | ConvertFrom-Json
    $h = $json.queue_handoff
    $gaps = $json.summary.gaps
    $rows += [pscustomobject]@{
        case = $case.Name
        exit = $code
        status = $json.result.status
        fps = $json.summary.fps
        queues = $h.queues_created
        handoff = $h.handoff
        signals = $h.fence_signals
        waits = $h.fence_waits
        last_queue = $h.last_queue
        clock = $h.queue_clock_witness
        clock_off_ms = $h.queue_clock_offset_ms
        q_b_hz = $h.queue_b_timestamp_frequency
        ge4ms = $gaps.count_ge_4ms
        ge4ms_s = $gaps.per_second_ge_4ms
        ge4ms_aligned = $gaps.vblank_aligned_ge_4ms
        ge1ms = $gaps.count_ge_1ms
        fitted = $gaps.vblank.fitted
        idle_after_p50 = $json.summary.gpu_ms.idle_after_submission_per_frame.p50
    }
    # The witness. 'many' means at least one a frame: a handoff arm that signalled nothing did not run.
    $frames = [int]$json.summary.frames_measured
    if ($null -eq $frames -or $frames -lt 1) { $frames = 1 }
    foreach ($pair in @(@{ What = 'signals'; Value = [int]$h.fence_signals; Want = $case.Signals },
                        @{ What = 'waits';   Value = [int]$h.fence_waits;   Want = $case.Waits })) {
        $ok = if ($pair.Want -eq 'zero') { $pair.Value -eq 0 } else { $pair.Value -ge $frames }
        if (-not $ok) {
            $failures++
            Write-Host ("FAIL  {0}: {1} {2}, wanted {3} (frames {4})" -f $case.Name, $pair.What, $pair.Value, $pair.Want, $frames)
        }
    }
    # The clock witness. A two-queue arm must say 'one domain', and it must carry the second queue's frequency
    # and an offset inside the 1 ms the client admits; a one-queue arm must say 'one queue' and nothing else.
    if ([string]$h.queue_clock_witness -ne $case.Clock) {
        $failures++
        Write-Host ("FAIL  {0}: clock witness '{1}', wanted '{2}'" -f $case.Name, $h.queue_clock_witness, $case.Clock)
    }
    if ($case.Clock -eq 'one domain') {
        if ([uint64]$h.queue_b_timestamp_frequency -lt 1) {
            $failures++
            Write-Host ("FAIL  {0}: the second queue reported no timestamp frequency" -f $case.Name)
        }
        if ([math]::Abs([double]$h.queue_clock_offset_ms) -ge 1.0) {
            $failures++
            Write-Host ("FAIL  {0}: clock offset {1} ms is outside the 1 ms the witness admits" -f $case.Name, $h.queue_clock_offset_ms)
        }
    } elseif ([uint64]$h.queue_b_timestamp_frequency -ne 0) {
        $failures++
        Write-Host ("FAIL  {0}: a one-queue arm reported a second queue's frequency" -f $case.Name)
    }
    if ($code -ne 0) { $failures++; Write-Host ("FAIL  {0}: exit {1}" -f $case.Name, $code) }
}
$rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Runs 'host-gpu-c48-table.json') -Encoding ascii
if ($failures) { "C48-HOST-RUNS FAIL $failures"; exit 1 }
"C48-HOST-RUNS PASS $($cases.Count) arms"
