# Host check of --ts-hz and the fitted clock origin (BD-056 interim lever).
#
# The point of the check: on this machine the driver's own GetClockCalibration is sound, so a run with --ts-hz
# set to the frequency the driver itself reports must come out the same as a run without it. That is the only
# place where the fit can be held against a known answer; on the lab there is no known answer until KMD 197.
#
#   powershell -ExecutionPolicy Bypass -File src\tsfit-runs.ps1 [-Seconds 8]
param([int]$Seconds = 8)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$here\work-dir.ps1"
$work = Get-FrameloopWork
$exe = Join-Path $work 'build\amdgpu_wddm_frameloop.exe'
$out = Join-Path $work 'runs'
$cases = @(
    @{ name = 'tsfit-gpu-plain';  args = @('--gpu-ms', '12', '--lists', '7') },
    @{ name = 'tsfit-gpu-right';  args = @('--gpu-ms', '12', '--lists', '7', '--ts-hz', '1e9') },
    @{ name = 'tsfit-gpu-wrong';  args = @('--gpu-ms', '12', '--lists', '7', '--ts-hz', '1e8') },
    @{ name = 'tsfit-present-plain'; args = @('--gpu-ms', '0', '--lists', '7') },
    @{ name = 'tsfit-present-right'; args = @('--gpu-ms', '0', '--lists', '7', '--ts-hz', '1e9') }
)
foreach ($case in $cases) {
    $json = Join-Path $out ($case.name + '.json')
    & $exe @('--seconds', $Seconds) @($case.args) @('--out', $json) | Out-Null
    $code = $LASTEXITCODE
    $d = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    $c = $d.summary.consistency
    $f = $d.clock_fit
    '{0,-22} exit {1} ratio {2,7:N4} usable {3,-5} neg {4}/{5} | fit {6,-5} bracket {7,6:N4} ms refits {8} clamped {9} driver off {10} ms | sub2start {11,7:N3} wake {12,7:N3}' -f @(
        $case.name, $code, $c.gpu_over_interval, $c.cross_clock_usable, $c.negative_submit_latency,
        $c.negative_wake_latency, $f.applied, $f.bracket_ms, $f.refits, $f.refits_clamped,
        $f.driver_point_offset_ms, $d.summary.latency_ms.submit_to_gpu_start.p50,
        $d.summary.latency_ms.gpu_end_to_wake_blocking.p50)
}
