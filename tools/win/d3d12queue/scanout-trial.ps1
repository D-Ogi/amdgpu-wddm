# Lab trial for M15.14: run the fullscreen flip client and read the kernel driver's scan-out counters around it.
#
# Runs on the lab, elevated, in one SSH session. The client's receipt says what D3D12 and DXGI returned; this
# script's own answer is the counter delta, which is the only place the driver says whether a buffer the
# application owns reached the display plane. Both are reported and neither is read as the other.
#
# The trial is bounded: -Seconds (default 70) is the client's own deadline, and every wait here is bounded by
# it. Three minutes is the standing limit for a lab trial that does not start a game.
#
#   pwsh -File scanout-trial.ps1 -Client C:\BC250\flip\amdgpu_wddm_d3d12_queue.exe `
#        -Cli C:\BC250\bc250kmd_cli.exe -Directory C:\BC250\tmp\flip-001 -Experiment scanout-flip
#
# -Experiment is the per-instance switch the D3D12 shell reads from AMDGPU_WDDM_D3D12_EXPERIMENT. Without it the
# shell keeps the registered composed-primary path, which is the control arm of this trial: the client must then
# present every frame exactly and the scan-out counters must not move.
param(
    [Parameter(Mandatory)][string]$Client,
    [Parameter(Mandatory)][string]$Cli,
    [Parameter(Mandatory)][string]$Directory,
    [string]$Experiment = '',
    [ValidateRange(20, 150)][int]$Seconds = 70,
    # Written next to the session directory; the trial's whole answer in one file.
    [string]$Report = ''
)
$ErrorActionPreference = 'Stop'
foreach ($path in $Client, $Cli) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "not found: $path" } }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
if (Test-Path -LiteralPath $root) { throw "session directory exists; every trial gets a new one: $root" }
New-Item -ItemType Directory -Force $root | Out-Null
if (-not $Report) { $Report = "$root.json" }

# The counter lines this trial reads, each with the names of the numbers in it. A line that the running driver
# does not print is reported as absent, never as zero: a missing line means an older KMD, not an idle one.
$patterns = [ordered]@{
    presents = @{
        regex  = 'presents (\d+), flips (\d+) of (\d+) address calls \((\d+) arrived above DISPATCH_LEVEL\)'
        names  = @('presents', 'flips', 'address_calls', 'above_dispatch')
    }
    scanout  = @{
        regex  = 'scan-out flips (\d+) of (\d+) requested candidates; admission ok/no-alloc/not-requested (\d+)/(\d+)/(\d+), format/geometry/pitch/size/segment/alignment (\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)'
        names  = @('scanout_flips', 'scanout_requests', 'admit_ok', 'admit_no_allocation', 'admit_not_requested',
                   'admit_format', 'admit_geometry', 'admit_pitch', 'admit_size', 'admit_segment', 'admit_alignment')
    }
    vidpn    = @{
        regex  = 'vidpn flip (\w+): (\d+) hardware flips, (\d+) refused'
        names  = @('vidpn_state', 'hardware_flips', 'hardware_refused')
    }
}
function Read-Counters {
    param([string]$Label)
    # "log summary" makes the WDDM table write its counters into the ring first, then reads the ring.
    $text = & $Cli log summary 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "bc250kmd_cli log summary failed ($LASTEXITCODE) at $Label" }
    $values = [ordered]@{}
    foreach ($key in $patterns.Keys) {
        $match = [regex]::Match($text, $patterns[$key].regex)
        if (-not $match.Success) { $values["${key}_line"] = 'absent'; continue }
        $i = 1
        foreach ($name in $patterns[$key].names) {
            $raw = $match.Groups[$i].Value
            $values[$name] = if ($raw -match '^\d+$') { [int64]$raw } else { $raw }
            $i++
        }
    }
    $file = Join-Path $root "summary-$Label.txt"
    Set-Content -LiteralPath $file -Value $text -Encoding ascii
    [pscustomobject]@{ label = $Label; utc = (Get-Date).ToUniversalTime().ToString('o'); values = $values; file = $file }
}

$before = Read-Counters 'before'
Write-Host "before: $($before.values | ConvertTo-Json -Compress)"

# The client reads the experiment switch from its own environment, so the arm is set here and nowhere else.
$previous = $env:AMDGPU_WDDM_D3D12_EXPERIMENT
if ($Experiment) { $env:AMDGPU_WDDM_D3D12_EXPERIMENT = $Experiment } else { Remove-Item Env:\AMDGPU_WDDM_D3D12_EXPERIMENT -ErrorAction SilentlyContinue }
$started = Get-Date
$deadline = $started.AddSeconds($Seconds + 10)
$stdout = Join-Path $root 'client.out'
$stderr = Join-Path $root 'client.err'
$process = Start-Process -FilePath $Client -ArgumentList @('--interactive', $root, '--deadline', $Seconds) -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden
try {
    $sequence = 0
    foreach ($command in 'create-device', 'create-queue', 'copy', 'status', 'exit') {
        $sequence++
        & (Join-Path $here 'controller.ps1') -Directory $root -Sequence $sequence -Command $command | Out-Null
        $result = Join-Path $root ('result-{0:d6}.json' -f $sequence)
        while (-not (Test-Path -LiteralPath $result) -and (Get-Date) -lt $deadline -and -not $process.HasExited) { Start-Sleep -Milliseconds 200 }
        if (-not (Test-Path -LiteralPath $result)) { throw "no receipt for $command within the trial's bound" }
        $receipt = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        Write-Host ("{0}: success {1} hr {2}" -f $command, $receipt.success, $receipt.hr)
        if (-not $receipt.success -and $command -ne 'status') { break }
    }
} finally {
    if ($Experiment) {
        if ($null -eq $previous) { Remove-Item Env:\AMDGPU_WDDM_D3D12_EXPERIMENT -ErrorAction SilentlyContinue }
        else { $env:AMDGPU_WDDM_D3D12_EXPERIMENT = $previous }
    }
    # A fullscreen window must never outlive its trial on the operator's screen.
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force; Write-Host 'client did not exit; stopped' }
}
$process.WaitForExit()
$exit = $process.ExitCode

# The counters are read after the client's process is gone, so no flip of this trial is still in flight.
$after = Read-Counters 'after'
Write-Host "after: $($after.values | ConvertTo-Json -Compress)"

$delta = [ordered]@{}
foreach ($name in $after.values.Keys) {
    if ($after.values[$name] -is [int64] -and $before.values[$name] -is [int64]) {
        $delta[$name] = $after.values[$name] - $before.values[$name]
    }
}
$session = Join-Path $root 'session.json'
$trace = Join-Path $root 'trace.jsonl'
$flipLine = if (Test-Path -LiteralPath $trace) { (Select-String -LiteralPath $trace -Pattern 'Fullscreen flip:' | Select-Object -Last 1).Line } else { $null }
$statistics = if (Test-Path -LiteralPath $trace) { (Select-String -LiteralPath $trace -Pattern 'Presented \d+ refused' | Select-Object -Last 1).Line } else { $null }
$report = [pscustomobject]@{
    schema      = 1
    utc         = $started.ToUniversalTime().ToString('o')
    client      = (Get-FileHash -LiteralPath $Client).Hash
    experiment  = $Experiment
    seconds     = $Seconds
    exit_code   = $exit
    session     = if (Test-Path -LiteralPath $session) { Get-Content -LiteralPath $session -Raw | ConvertFrom-Json } else { $null }
    flip_result = $flipLine
    statistics  = $statistics
    before      = $before.values
    after       = $after.values
    delta       = $delta
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Report -Encoding ascii
Write-Host "delta: $($delta | ConvertTo-Json -Compress)"
Write-Host "report: $Report"

# What the trial shows, said in one line, and nothing beyond what the numbers carry.
if ($delta.Count -eq 0) { Write-Host 'VERDICT inconclusive: the running driver prints no counter line this trial can read' }
elseif (-not $Experiment) {
    $quiet = $delta.scanout_requests -eq 0 -and $delta.scanout_flips -eq 0
    Write-Host ("VERDICT control arm: scan-out requests {0}, flips {1} - {2}" -f $delta.scanout_requests, $delta.scanout_flips,
        $(if ($quiet) { 'the registered path is unchanged, as it must be' } else { 'UNEXPECTED: the registered path asked for scan-out' }))
}
elseif ($delta.scanout_flips -gt 0) {
    Write-Host ("VERDICT scan-out: {0} of {1} address calls flipped an application buffer ({2} candidates requested)" -f
        $delta.scanout_flips, $delta.address_calls, $delta.scanout_requests)
}
elseif ($delta.scanout_requests -gt 0) {
    $refusals = @()
    foreach ($name in 'admit_not_requested', 'admit_format', 'admit_geometry', 'admit_pitch', 'admit_size', 'admit_segment', 'admit_alignment', 'admit_no_allocation') {
        if ($delta[$name] -gt 0) { $refusals += "$name $($delta[$name])" }
    }
    Write-Host ("VERDICT refused: {0} candidates reached the kernel driver and none was admitted ({1})" -f $delta.scanout_requests, ($refusals -join ', '))
}
else {
    Write-Host 'VERDICT not reached: no scan-out candidate reached SetVidPnSourceAddress; the request stopped in user mode'
}
if ($exit -ne 0) { exit 1 }
