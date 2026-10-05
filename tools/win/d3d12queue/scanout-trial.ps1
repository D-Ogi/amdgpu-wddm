# Lab trial for M15.14: run the fullscreen flip client and read the kernel driver's scan-out counters around it.
#
# Runs on the lab, elevated, from one SSH session. The client's receipt says what D3D12 and DXGI returned; this
# script's own answer is the counter delta, which is the only place the driver says whether a buffer the
# application owns reached the display plane. Both are reported and neither is read as the other.
#
# The client runs as a scheduled task of the interactive session, not from here: an SSH session on this lab is
# elevated but lives in session 0, where a window is on no monitor and is composed by no interactive DWM, so a
# flip of it could never be independent and the operator could never see it. The task uses the pattern this lab
# already uses for an interactive client (New-ScheduledTaskPrincipal -LogonType Interactive), and its name stays
# clear of the deploy kits' competing-task pattern (BC250|DWM|G0|WSI).
#
# The trial is bounded: -Seconds (default 70) is the client's own deadline, and every wait here is bounded by
# it. Three minutes is the standing limit for a lab trial that does not start a game.
#
#   pwsh -File scanout-trial.ps1 -Client C:\BC250\flip\amdgpu_wddm_d3d12_queue.exe `
#        -Cli C:\BC250\bc250kmd_cli.exe -Directory C:\BC250\tmp\flip-001 -Experiment scanout-flip-1920x1200
#
# -Experiment is the per-instance switch the D3D12 shell reads from AMDGPU_WDDM_D3D12_EXPERIMENT. The scan-out
# mode names the geometry it is for, because the kernel driver admits a flip at the POST geometry alone. Without
# -Experiment the shell keeps the registered composed-primary path, which is the control arm of this trial: the
# client must then present every frame exactly and the scan-out counters must not move.
param(
    [string]$Client,
    [string]$Cli,
    [string]$Directory,
    [string]$Experiment = '',
    [ValidateRange(20, 150)][int]$Seconds = 70,
    # Frames and back buffers, read by the client from its own environment, so both arms and both chain depths
    # are the same executable.
    [ValidateRange(1, 20000)][int]$Frames = 600,
    [ValidateRange(2, 3)][int]$Buffers = 3,
    # The interactive account the client runs as, and the task that carries it.
    [string]$User = 'bc250',
    [string]$TaskName = 'Scanout flip trial',
    # The overlay's own `log summary` poll costs a measured ~300 ms game stall every ~5.4 s; its pause file
    # stops it for the length of the trial and is removed again afterwards if this script created it.
    [string]$OverlayPause = 'C:\BC250\mon\graphics-summary.pause',
    # Written next to the session directory; the trial's whole answer in one file.
    [string]$Report = '',
    # Parse and judge sample counter texts and exit. Touches no file, no task and no driver: this is the gate
    # for the verdict logic itself, which is where three defects of the first increment were.
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
if (-not $SelfTest) {
    foreach ($path in $Client, $Cli) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "not found: $path" } }
}
if ($Experiment -and ($Experiment -split ',') -contains 'scanout-flip') {
    throw 'the scan-out mode names its geometry: scanout-flip-<width>x<height>, e.g. scanout-flip-1920x1200'
}
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = ''
if (-not $SelfTest) {
    if (-not $Client -or -not $Cli -or -not $Directory) { throw 'Client, Cli and Directory are required unless -SelfTest' }
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    if (Test-Path -LiteralPath $root) { throw "session directory exists; every trial gets a new one: $root" }
    New-Item -ItemType Directory -Force $root | Out-Null
    if (-not $Report) { $Report = "$root.json" }
}

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
function Parse-Counters {
    param([string]$Text, [string]$Label)
    $values = [ordered]@{}
    foreach ($key in $patterns.Keys) {
        $match = [regex]::Match($Text, $patterns[$key].regex)
        if (-not $match.Success) { $values["${key}_line"] = 'absent'; continue }
        $i = 1
        foreach ($name in $patterns[$key].names) {
            $raw = $match.Groups[$i].Value
            $values[$name] = if ($raw -match '^\d+$') { [int64]$raw } else { $raw }
            $i++
        }
    }
    [pscustomobject]@{ label = $Label; utc = (Get-Date).ToUniversalTime().ToString('o'); values = $values; file = $null }
}
function Read-Counters {
    param([string]$Label)
    # "log summary" makes the WDDM table write its counters into the ring first, then reads the ring.
    $text = & $Cli log summary 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "bc250kmd_cli log summary failed ($LASTEXITCODE) at $Label" }
    $read = Parse-Counters $text $Label
    $read.file = Join-Path $root "summary-$Label.txt"
    Set-Content -LiteralPath $read.file -Value $text -Encoding ascii
    return $read
}
# A counter is usable only when both reads carried it. An absent line must never be read as a zero delta: the
# first baseline of this trial is the deployed 0.7.205.1, which has no scan-out counters at all, and a zero
# there would otherwise be printed as "the registered path is unchanged, as it must be".
function Has-Counter {
    param([pscustomobject]$Before, [pscustomobject]$After, [string]$Name)
    return $Before.values.Contains($Name) -and $After.values.Contains($Name) -and
           $Before.values[$Name] -is [int64] -and $After.values[$Name] -is [int64]
}

# What the trial shows, said in one line, and nothing beyond what the numbers carry. admit_ok is deliberately
# absent from every verdict: it counts every SetVidPnSourceAddress with an allocation, including the
# compositor's own primary, so over a minute its delta is thousands of DWM flips and the client's frames are a
# rounding error in it. The client-specific numbers are scanout_requests and scanout_flips.
#
# A function, and driven by -SelfTest over sample summary texts, because each of the three verdicts has already
# been wrong once: an absent counter read as a zero, admit_ok used as the client's own number, and a scan-out
# flip counted with the flip gate closed.
function Write-Verdict {
    # Returns the verdict lines rather than printing them: -SelfTest reads them back, and the trial prints
    # what it gets. A verdict nothing can read is a verdict nothing can test.
    param([pscustomobject]$Before, [pscustomobject]$After, $Delta, [string]$Arm)
    $haveScanout = (Has-Counter $Before $After 'scanout_flips') -and (Has-Counter $Before $After 'scanout_requests')
    $haveFlips = Has-Counter $Before $After 'flips'
    $haveHardware = Has-Counter $Before $After 'hardware_flips'
    if (-not $haveScanout) {
        Write-Output 'VERDICT inconclusive: the running driver prints no scan-out counter line, so this trial can say nothing about scan-out (KMD below 0.7.206.1)'
    }
    elseif (-not $Arm) {
        $quiet = $Delta.scanout_requests -eq 0 -and $Delta.scanout_flips -eq 0
        $alive = $haveFlips -and $Delta.flips -gt 0 -and $Delta.address_calls -gt 0
        Write-Output ("VERDICT control arm: scan-out requests {0}, flips {1}; {2} flips of {3} address calls overall - {4}" -f
            $Delta.scanout_requests, $Delta.scanout_flips, $Delta.flips, $Delta.address_calls,
            $(if ($quiet -and $alive) { 'the registered path is unchanged and still flipping, as it must be' }
              elseif ($quiet) { 'quiet, but the overall flip counters did not move either: check that the desktop was composing at all' }
              else { 'UNEXPECTED: the registered path asked for scan-out' }))
    }
    elseif ($Delta.scanout_flips -gt 0) {
        # A scan-out flip is counted only when DcnFlipSourceAddress succeeded, so hardware_flips must move with it.
        # If it does not, the flip gate is closed or the write sequence failed, and the headline number is not one.
        $hardwareMoved = $haveHardware -and $Delta.hardware_flips -ge $Delta.scanout_flips
        Write-Output ("VERDICT scan-out: {0} of {1} flips were an application buffer ({2} candidates requested, {3} hardware flips, {4} refused by the hardware) - {5}" -f
            $Delta.scanout_flips, $Delta.flips, $Delta.scanout_requests, $Delta.hardware_flips, $Delta.hardware_refused,
            $(if ($hardwareMoved) { 'confirmed at the display hardware' } else { 'NOT CONFIRMED: the hardware flip counter did not move with it, so check EnableVidPnFlip and the VUPDATE sequence' }))
    }
    elseif ($Delta.scanout_requests -gt 0) {
        $refusals = @()
        foreach ($name in 'admit_not_requested', 'admit_format', 'admit_geometry', 'admit_pitch', 'admit_size', 'admit_segment', 'admit_alignment', 'admit_no_allocation') {
            if ($Delta.Contains($name) -and $Delta[$name] -gt 0) { $refusals += "$name $($Delta[$name])" }
        }
        Write-Output ("VERDICT refused: {0} candidates reached the kernel driver and none was admitted ({1})" -f $Delta.scanout_requests, ($refusals -join ', '))
    }
    else {
        Write-Output 'VERDICT not reached: no scan-out candidate reached SetVidPnSourceAddress, so the stop is above the kernel driver.'
        Write-Output '  For a borderless chain that layer is the compositor: DWM decides DirectFlip through its own UMD, which has no CheckDirectFlipSupport entry (see docs/design/scanout-admission.md, "What is still missing").'
        Write-Output '  For an exclusive-fullscreen chain it is DXGI or the shell: check the client trace for GetFullscreenState and whether the allocation carried the SCANOUT record.'
    }
}

# ---- -SelfTest: the parsing and the verdicts, over sample texts, with no lab ------------------------------
if ($SelfTest) {
    $v0 = @'
wddm summary: presents 400, flips 4000 of 4001 address calls (0 arrived above DISPATCH_LEVEL)
vidpn flip on: 4000 hardware flips, 0 refused
'@
    $v1 = @'
wddm summary: presents 400, flips 4000 of 4001 address calls (0 arrived above DISPATCH_LEVEL)
wddm summary: scan-out flips 0 of 0 requested candidates; admission ok/no-alloc/not-requested 3900/0/0, format/geometry/pitch/size/segment/alignment 0/0/0/0/0/0
vidpn flip on: 4000 hardware flips, 0 refused
'@
    $v2 = @'
wddm summary: presents 520, flips 4600 of 4601 address calls (0 arrived above DISPATCH_LEVEL)
wddm summary: scan-out flips 600 of 600 requested candidates; admission ok/no-alloc/not-requested 4500/0/0, format/geometry/pitch/size/segment/alignment 0/0/0/0/0/0
vidpn flip on: 4600 hardware flips, 0 refused
'@
    $gated = @'
wddm summary: presents 520, flips 4600 of 4601 address calls (0 arrived above DISPATCH_LEVEL)
wddm summary: scan-out flips 0 of 600 requested candidates; admission ok/no-alloc/not-requested 4500/0/0, format/geometry/pitch/size/segment/alignment 0/0/0/0/600/0
vidpn flip on: off: 0 hardware flips, 0 refused
'@
    $failures = 0
    function Judge { param($BeforeText, $AfterText, $Arm)
        $b = Parse-Counters $BeforeText 'before'
        $a = Parse-Counters $AfterText 'after'
        $d = [ordered]@{}
        foreach ($key in $patterns.Keys) { foreach ($name in $patterns[$key].names) {
            if (Has-Counter $b $a $name) { $d[$name] = $a.values[$name] - $b.values[$name] } } }
        return (Write-Verdict $b $a $d $Arm | Out-String)
    }
    function Expect { param([string]$What, [string]$Text, [string]$Pattern)
        if ($Text -notmatch $Pattern) { $script:failures++; Write-Host "FAIL $What : expected /$Pattern/ in: $($Text.Trim())" }
        else { Write-Host "ok   $What" } }
    # A KMD without the scan-out line says nothing about scan-out, in either arm. This is the one that used to
    # print "UNEXPECTED: the registered path asked for scan-out" against the deployed 0.7.205.1.
    Expect 'control arm, no scan-out counters' (Judge $v0 $v0 '') 'VERDICT inconclusive'
    Expect 'scan-out arm, no scan-out counters' (Judge $v0 $v0 'scanout-flip-1920x1200') 'VERDICT inconclusive'
    # The control arm: the counters exist, nothing asked, and the desktop went on flipping.
    Expect 'control arm quiet and alive' (Judge $v1 $v2.Replace('600 of 600', '0 of 0').Replace('scan-out flips 0', 'scan-out flips 0') ) 'the registered path is unchanged and still flipping'
    # admit_ok moves by 600 in both arms and must never be the number a verdict rests on.
    Expect 'control arm does not read admit_ok' (Judge $v1 $v2.Replace('600 of 600', '0 of 0').Replace('4500/0/0', '9000/0/0')) 'the registered path is unchanged and still flipping'
    # The scan-out arm, confirmed at the hardware.
    Expect 'scan-out confirmed' (Judge $v1 $v2 'scanout-flip-1920x1200') 'confirmed at the display hardware'
    # The same scan-out flips with no hardware flip behind them: the flip gate was closed.
    Expect 'scan-out not confirmed' (Judge $v1 $v2.Replace('vidpn flip on: 4600', 'vidpn flip on: 4000') 'scanout-flip-1920x1200') 'NOT CONFIRMED'
    # Candidates that reached the kernel driver and were refused, with the clause named.
    Expect 'refusal named' (Judge $v1 $gated 'scanout-flip-1920x1200') 'VERDICT refused.*admit_segment 600'
    # Nothing reached the kernel driver: the verdict names the layer above it, and names DWM.
    Expect 'not reached names DWM' (Judge $v1 $v1 'scanout-flip-1920x1200') 'VERDICT not reached'
    Expect 'not reached explains DWM' (Judge $v1 $v1 'scanout-flip-1920x1200') 'DWM decides DirectFlip'
    # The geometry has to be named: a bare scanout-flip is refused before anything runs.
    try { & $PSCommandPath -SelfTest:$false -Client $PSCommandPath -Cli $PSCommandPath -Directory (Join-Path $env:TEMP 'scanout-selftest-unused') -Experiment 'scanout-flip' | Out-Null
          $failures++; Write-Host 'FAIL bare scanout-flip was accepted' }
    catch { if ($_.Exception.Message -match 'names its geometry') { Write-Host 'ok   bare scanout-flip refused' }
            else { $failures++; Write-Host "FAIL bare scanout-flip: $($_.Exception.Message)" } }
    Write-Host ("scanout-trial self-test: {0} failure(s)" -f $failures)
    exit ($failures ? 1 : 0)
}

$before = Read-Counters 'before'
Write-Host "before: $($before.values | ConvertTo-Json -Compress)"

# The client's environment travels in the wrapper the task runs, because a scheduled task does not inherit this
# session's variables. Written with ASCII and no quoting surprises: paths on this lab have no spaces.
$stdout = Join-Path $root 'client.out'
$stderr = Join-Path $root 'client.err'
$wrapper = Join-Path $root 'run-client.cmd'
$lines = @('@echo off')
if ($Experiment) { $lines += "set AMDGPU_WDDM_D3D12_EXPERIMENT=$Experiment" }
else { $lines += 'set AMDGPU_WDDM_D3D12_EXPERIMENT=' }
$lines += "set AMDGPU_WDDM_D3D12_FLIP_FRAMES=$Frames"
$lines += "set AMDGPU_WDDM_D3D12_FLIP_BUFFERS=$Buffers"
$lines += "`"$Client`" --interactive `"$root`" --deadline $Seconds > `"$stdout`" 2> `"$stderr`""
$lines += 'echo client_exit %ERRORLEVEL%>> "' + $stdout + '"'
Set-Content -LiteralPath $wrapper -Value $lines -Encoding ascii

$pausedByUs = $false
if ($OverlayPause -and -not (Test-Path -LiteralPath $OverlayPause)) {
    $parent = Split-Path -Parent $OverlayPause
    if (Test-Path -LiteralPath $parent) {
        Set-Content -LiteralPath $OverlayPause -Value "scanout-trial $([DateTime]::UtcNow.ToString('o'))" -Encoding ascii
        $pausedByUs = $true
    }
}
$started = Get-Date
$deadline = $started.AddSeconds($Seconds + 20)
$action = New-ScheduledTaskAction -Execute $wrapper -WorkingDirectory $root
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Force | Out-Null
$taskState = 'not started'
try {
    Start-ScheduledTask -TaskName $TaskName
    $sequence = 0
    foreach ($command in 'create-device', 'create-queue', 'copy', 'status', 'exit') {
        $sequence++
        & (Join-Path $here 'controller.ps1') -Directory $root -Sequence $sequence -Command $command | Out-Null
        $result = Join-Path $root ('result-{0:d6}.json' -f $sequence)
        while (-not (Test-Path -LiteralPath $result) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
        if (-not (Test-Path -LiteralPath $result)) { throw "no receipt for $command within the trial's bound" }
        $receipt = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        Write-Host ("{0}: success {1} hr {2}" -f $command, $receipt.success, $receipt.hr)
        if (-not $receipt.success -and $command -ne 'status') { break }
    }
} finally {
    # A fullscreen window must never outlive its trial on the operator's screen, and neither may the task.
    while ((Get-Date) -lt $deadline) {
        $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        if (-not $task -or $task.State -ne 'Running') { break }
        Start-Sleep -Milliseconds 300
    }
    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($task) {
        $taskState = [string]$task.State
        if ($task.State -eq 'Running') { Stop-ScheduledTask -TaskName $TaskName; Write-Host 'client did not exit; task stopped' }
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    }
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Client)) -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.Id -Force; Write-Host "stopped leftover client $($_.Id)" }
    if ($pausedByUs) { Remove-Item -LiteralPath $OverlayPause -ErrorAction SilentlyContinue }
}
$exitLine = if (Test-Path -LiteralPath $stdout) { (Select-String -LiteralPath $stdout -Pattern 'client_exit (\d+)' | Select-Object -Last 1) } else { $null }
$exit = if ($exitLine) { [int]$exitLine.Matches[0].Groups[1].Value } else { -1 }

# The counters are read after the client's process is gone, so no flip of this trial is still in flight.
$after = Read-Counters 'after'
Write-Host "after: $($after.values | ConvertTo-Json -Compress)"

$delta = [ordered]@{}
$missing = @()
foreach ($key in $patterns.Keys) {
    foreach ($name in $patterns[$key].names) {
        if (Has-Counter $before $after $name) { $delta[$name] = $after.values[$name] - $before.values[$name] }
        elseif ($after.values.Contains($name) -and $after.values[$name] -is [string]) { }  # a text field, e.g. vidpn_state
        else { $missing += $name }
    }
}
$session = Join-Path $root 'session.json'
$trace = Join-Path $root 'trace.jsonl'
function Trace-Line { param([string]$Pattern)
    if (Test-Path -LiteralPath $trace) { (Select-String -LiteralPath $trace -Pattern $Pattern | Select-Object -Last 1).Line } else { $null } }
$report = [pscustomobject]@{
    schema       = 2
    utc          = $started.ToUniversalTime().ToString('o')
    client       = (Get-FileHash -LiteralPath $Client).Hash
    experiment   = $Experiment
    seconds      = $Seconds
    frames       = $Frames
    buffers      = $Buffers
    task_state   = $taskState
    exit_code    = $exit
    session      = if (Test-Path -LiteralPath $session) { Get-Content -LiteralPath $session -Raw | ConvertFrom-Json } else { $null }
    plan         = Trace-Line 'Flip plan:'
    flip_result  = Trace-Line 'Fullscreen flip:'
    statistics   = Trace-Line 'Presented \d+ refused'
    render_window = Trace-Line 'Render window'
    before       = $before.values
    after        = $after.values
    delta        = $delta
    missing      = $missing
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Report -Encoding ascii
Write-Host "delta: $($delta | ConvertTo-Json -Compress)"
Write-Host "report: $Report"

Write-Verdict $before $after $delta $Experiment | ForEach-Object { Write-Host $_ }
if ($exit -ne 0) { exit 1 }
