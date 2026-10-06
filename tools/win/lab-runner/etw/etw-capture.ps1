# Lab-side bounded ETW CPU profile of a game trial (owner request via Codex 848: use the collected profilers).
# Runs as a one-shot SYSTEM task started by etw-start.ps1 right after the trial's runtime starts. Two windows:
#   A: $StartA s after the game appears, $Seconds long (owner: DWM misbehaves from the game start). The game is
#      -Process (comma-separated Get-Process names, wildcards allowed; default witcher3, game runner profiles).
#   B: when dwm.exe CPU exceeds $DwmPct % of the machine in two consecutive 2 s samples, or at $LatestB s after
#      the game appeared, whichever first.
# One deadline bounds everything: $NotAfterQpc is the trial's own runtime cutoff (watch.ps1 origin + 255 s, less a
# margin, computed by etw-start.ps1 from the trial's start.json) on the shared QPC clock (Codex 854). A window is
# only started when its collection plus the cleanup reserve fits before it, and every wait, logman call and
# helper exit is cut to what is left; an exit that cannot be verified in time counts as a failed cleanup.
# Each window owns the sessions named BC250-<Tag>-<W>* (PerfView's own pair plus the DxgKrnl one); cleanup of
# owned sessions runs in finally, and a failed session query is never taken as absence (Codex 850). The script
# exits 1 when any cleanup failed; task exit alone is still not a closure witness: the operator's etw-closure.ps1,
# run once this task is no longer Running, is the independent receipt.
# FPS mode (owner, 2026-09-30: Full HD frame-rate tests): -GpuOnly records only the DxgKrnl session (no PerfView
# CPU profile, so the measurement does not load the CPU and stops in seconds); -SkipA drops window A; -WorldLog
# starts window B when the game log shows the walk phase ("walk Ns: start", the world is up) and B then lasts
# -SecondsB or what the deadline leaves, whichever is shorter.
# Present-mode mode (M15.14): -PresentMode adds Microsoft-Windows-Win32k and Microsoft-Windows-Dwm-Core to the
# DxgKrnl session, with narrow keyword masks, so that etw-present-mode.py can say per frame whether a present was
# scanned out or composed. It is off by default and changes nothing else about a window.
param([string]$Root, [string]$Tag, [int]$Seconds = 30, [int]$StartA = 5, [int]$DwmPct = 15, [int]$LatestB = 110,
    [long]$NotAfterQpc = 0, [switch]$Smoke, [switch]$GpuOnly, [switch]$SkipA, [string]$WorldLog = '', [int]$SecondsB = 0,
    [int]$ReserveSeconds = 0, [string]$Process = 'witcher3', [switch]$PresentMode)
$ErrorActionPreference = 'Stop'
$gameNames = @($Process -split ',' | Where-Object { $_ })
$perf = 'C:\BC250\tools\perfview\PerfView.exe'
$env:PerfView_APPDATA = 'C:\BC250\tools\perfview\appdata'
$env:TEMP = Join-Path $Root 'tmp'; $env:TMP = $env:TEMP
$null = New-Item -ItemType Directory -Force -Path $Root, $env:TEMP
$notes = Join-Path $Root 'etw-notes.txt'
$freq = [Diagnostics.Stopwatch]::Frequency
if ($NotAfterQpc -le 0) {
    if (!$Smoke) { throw 'NotAfterQpc is required outside -Smoke' }
    $NotAfterQpc = [Diagnostics.Stopwatch]::GetTimestamp() + 120 * $freq
}
$reserve = 45     # seconds kept for stopping and verifying a window's sessions
if ($GpuOnly) { $reserve = 15 }   # one logman stop and two queries (155: stop 1.7 s, queries under 1 s)
# -ReserveSeconds overrides it (not -Reserve: PowerShell variable names are case-insensitive, so 163 lost the value to $reserve = 45) (162: the world came with 55 s left and the 45 s reserve refused the CPU window; 156's full
# window B needed 5.5 s from the collector's end to "sessions gone").
if ($ReserveSeconds -gt 0) { $reserve = $ReserveSeconds }
$t0 = $null
$script:cleanupFailed = $false
function Note([string]$s) {
    $rel = if ($t0) { '{0:0.0}' -f ([DateTime]::UtcNow - $t0).TotalSeconds } else { '-' }
    Add-Content -LiteralPath $notes -Value ('{0} t={1} left={2:0.0} {3}' -f [DateTime]::UtcNow.ToString('o'), $rel, (Left), $s)
}
function Left { return ($NotAfterQpc - [Diagnostics.Stopwatch]::GetTimestamp()) / [double]$freq }
# Waits for $p at most $cap seconds and never past the deadline; kills it on expiry and waits for the kill only
# within the remaining budget. Returns 'exited', 'killed' or 'unverified' (still alive, or exit not witnessed).
function Wait-Exit($p, [double]$cap) {
    $ms = [int]([Math]::Max(0, [Math]::Min($cap, (Left))) * 1000)
    if ($p.WaitForExit($ms)) { return 'exited' }
    try { $p.Kill() } catch {}
    $ms = [int]([Math]::Max(0, [Math]::Min(5, (Left))) * 1000)
    if ($p.WaitForExit($ms)) { return 'killed' }
    return 'unverified'
}
# One bounded run of a console tool with its output in $out. Returns @{ ok; exit; how } where ok means it exited
# by itself within the budget; nothing runs once the budget is gone. A helper that outlives its Kill is unjoined:
# it is noted with its PID, cleanup counts as failed, and no further helper is started (Codex 865).
$script:unjoined = $null
function Invoke-Tool([string]$exe, [string[]]$argv, [double]$cap, [string]$out) {
    if ($script:unjoined) { return @{ ok = $false; exit = $null; how = "blocked by unjoined pid $($script:unjoined)" } }
    if ((Left) -lt 1) { return @{ ok = $false; exit = $null; how = 'no-budget' } }
    $p = Start-Process -FilePath $exe -ArgumentList $argv -RedirectStandardOutput $out -RedirectStandardError "$out.err" -NoNewWindow -PassThru
    $null = $p.Handle   # PS 5.1: without a cached handle ExitCode can read back empty after a fast exit
    $how = Wait-Exit $p $cap
    Latch $how $p "$exe $($argv -join ' ')"
    if ($how -ne 'exited') { return @{ ok = $false; exit = $null; how = $how } }
    return @{ ok = $true; exit = $p.ExitCode; how = $how }
}
# Every launch, direct ones included, records an exit it could not witness here (Codex 866).
function Latch([string]$how, $p, [string]$what) {
    if ($how -ne 'unverified') { return }
    $script:cleanupFailed = $true
    if (!$script:unjoined) { $script:unjoined = $p.Id }
    Note "helper $what pid $($p.Id) not joined after Kill"
}
# Owned sessions by name prefix: @{ ok = query succeeded; names = matching session names }.
$script:queries = 0
function Owned([string]$prefix) {
    # 170: one query failed with "The GUID passed was not recognized as valid by a WMI data provider" (exit
    # -2147020696) while the same query succeeded a minute later; a transient WMI error must not cost the window,
    # so the query is retried twice, 3 s apart, before it is reported as failed.
    $why = ''
    for ($try = 1; $try -le 3; $try++) {
        $script:queries++
        $out = Join-Path $env:TEMP "logman-query-$($script:queries).txt"
        $r = Invoke-Tool 'logman.exe' @('query', '-ets') 15 $out
        if ($r.ok -and $r.exit -eq 0) {
            return @{ ok = $true; names = @(Get-Content -LiteralPath $out | ForEach-Object { ($_ -split '\s+')[0] } | Where-Object { $_ -like "$prefix*" }) }
        }
        $why = "$($r.how) exit $($r.exit) (try $try)"
        Note "session query failed: $why"
        Start-Sleep -Seconds 3
    }
    return @{ ok = $false; names = @(); why = $why }
}
function Stop-Owned([string]$w, [string]$session) {
    $q = Owned $session
    if (!$q.ok) { Note "window $w cleanup: session query failed ($($q.why)), closure NOT established"; return $false }
    foreach ($n in $q.names) {
        if ($n -like '*-gpu') {
            $r = Invoke-Tool 'logman.exe' @('stop', $n, '-ets') 20 (Join-Path $Root "gpu-$w-stop.log")
            Note "window $w stop $n $($r.how) exit $($r.exit)"
        }
    }
    $q = Owned $session
    if ($q.ok -and $q.names.Count) {
        $slog = Join-Path $Root "perfview-$w-stop.log"
        if ($script:unjoined) { Note "window $w named stop skipped: unjoined pid $($script:unjoined)" }
        elseif ((Left) -ge 1) {
            $s = Start-Process -FilePath $perf -ArgumentList @("/LogFile:$slog", '/NoGui', '/NoView', '/AcceptEULA',
                "/SessionName:$session", '/NoClrRundown', '/NoNGenRundown', '/NoNGenPdbs', '/Merge:false', '/Zip:false',
                'stop') -PassThru -WindowStyle Hidden
            $how = Wait-Exit $s ([Math]::Min(40, (Left) - 10))
            Note "window $w named stop $how"
            Latch $how $s 'PerfView stop'
        }
        $q = Owned $session
        if ($q.ok) {
            foreach ($n in $q.names) {
                $r = Invoke-Tool 'logman.exe' @('stop', $n, '-ets') 15 (Join-Path $env:TEMP "logman-stop-$n.txt")
                Note "window $w logman stop $n $($r.how) exit $($r.exit)"
            }
            $q = Owned $session
        }
    }
    if (!$q.ok) { Note "window $w cleanup: final session query failed ($($q.why)), closure NOT established"; return $false }
    if ($q.names.Count) { Note "window $w cleanup FAILED, still listed: $($q.names -join ',')"; return $false }
    if ($script:unjoined) { Note "window $w cleanup unproven: unjoined pid $($script:unjoined)"; return $false }
    Note "window $w sessions gone (query ok)"
    return $true
}
function Capture([string]$w, [int]$secs = $Seconds) {
    $session = "BC250-$Tag-$w"
    if ((Left) -lt $secs + $reserve) { Note ("window $w skipped: {0:0} s left before the deadline" -f (Left)); return }
    $q = Owned $session
    if (!$q.ok) { Note "window $w skipped: session query failed ($($q.why))"; return }
    if ($q.names.Count) { Note "window $w skipped: sessions already exist: $($q.names -join ',')"; return }
    $etl = Join-Path $Root "perf-$w.etl"
    $log = Join-Path $Root "perfview-$w.log"
    $pvArgs = @("/LogFile:$log", '/NoGui', '/NoView', '/AcceptEULA', "/SessionName:$session", '/ThreadTime',
        '/ClrEvents:None',
        '/KernelEvents:Process,Thread,ImageLoad,Profile,ContextSwitch,Dispatcher,MemoryHardFaults,DiskIO,DiskIOInit,DiskFileIO',
        '/BufferSizeMB:32', '/CircularMB:128', "/MaxCollectSec:$secs", '/NoClrRundown', '/NoNGenRundown',
        '/NoNGenPdbs', '/Merge:false', '/Zip:false', 'collect', $etl)
    # GPU side (owner: what exactly does a frame wait for): DxgKrnl queue packets, DMA packets and VSync/flip in a
    # separate logman session, same shape as the G0 DWM trials (all keywords, level 5, 1 MB buffers, 64-256),
    # circular at 256 MB: up to 256 MB of buffers on top of PerfView's 32 MB request (recorded, Codex 850).
    $gpu = "$session-gpu"
    # 161 (3-minute FPS window): 159 wrote 2.4 MB/s, so a GPU-only window longer than 90 s gets 3 MB per second plus
    # 64 MB instead of the 256 MB circle, which would otherwise overwrite the window's start. 257 (LOW at 53 fps with
    # two presenting processes) wrote ~4.4 MB/s and lost its first 90 s to the 1054 MB circle: 6 MB per second now.
    # 397-399: RotTR D3D12 wrote ~19 MB/s and W3 LOW ~4.3 MB/s, so the fixed 256 MB circle of windows up to 90 s lost
    # their first 7-32 s (no event of any process there, read once as a game stall). Size every window for 20 MB/s,
    # capped at 4 GB (P: and the lab disk); the analysis reports the first present, so a wrapped circle still shows.
    $perSecond = 20
    # -PresentMode: two more providers in the same session. The keyword masks are the narrowest that still carry
    # what a present-mode classifier needs, read off the running system (logman query providers), and the level is
    # Informational, not Verbose:
    #   Win32k 0x0000100000001000 = Updates | Composition - the flip-chain and composition-surface tokens that
    #       say which buffer the compositor took, and nothing of input, fonts, handles or messages;
    #   Dwm-Core 0x0000000000000289 = Composition | DwmFrameRate | Scheduling | Overlays - what DWM scheduled and
    #       presented, without DetailedFrameInformation, which is the one high-volume keyword of that provider.
    # Both are per-frame events of one compositor and one game, so the sizing goes from 20 to 28 MB per second
    # (measured per-second rates behind the 20: RotTR D3D12 ~19 MB/s, W3 LOW ~4.3 MB/s of DxgKrnl alone), and the
    # buffer count is doubled so that three providers in one session do not drop events between flushes.
    $providerFile = $null
    if ($PresentMode) {
        $perSecond = 28
        $providerFile = Join-Path $Root "gpu-$w-providers.txt"
        Set-Content -LiteralPath $providerFile -Encoding ascii -Value @(
            '{802EC45A-1E99-4B83-9920-87C98277BA9D} 0xffffffffffffffff 0x5 Microsoft-Windows-DxgKrnl',
            '{8C416C79-D49B-4F01-A467-E56D3AA8234C} 0x0000100000001000 0x4 Microsoft-Windows-Win32k',
            '{9E9BBA3C-2E38-40CB-99F4-9E8281425164} 0x0000000000000289 0x4 Microsoft-Windows-Dwm-Core')
    }
    $maxMb = [Math]::Min(4096, [Math]::Max(256, $perSecond * $secs + 64))
    $buffers = if ($PresentMode) { @('-nb', '128', '512') } else { @('-nb', '64', '256') }
    $providers = if ($PresentMode) { @('-pf', $providerFile) } else { @('-p', 'Microsoft-Windows-DxgKrnl', '0xffffffffffffffff', '5') }
    try {
        $r = Invoke-Tool 'logman.exe' (@('start', $gpu, '-ets', '-o', (Join-Path $Root "gpu-$w.etl")) + $providers +
            @('-f', 'bincirc', '-max', "$maxMb", '-bs', '1024') + $buffers) 20 (Join-Path $Root "gpu-$w-start.log")
        Note "window $w gpu session $gpu start $($r.how) exit $($r.exit) providers $(if ($PresentMode) { 'dxgkrnl+win32k+dwm-core' } else { 'dxgkrnl' }) max ${maxMb}MB"
        if ($script:unjoined) { Note "window $w collector not started: unjoined pid $($script:unjoined)"; return }
        Note "window $w start session=$session seconds=$secs gpu_only=$([bool]$GpuOnly)"
        if ($GpuOnly) {
            # Only the DxgKrnl session runs: hold it for $secs, never into the reserve, ending early with the game.
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ($sw.Elapsed.TotalSeconds -lt $secs -and (Left) -gt $reserve -and
                   (Get-Process -Name $gameNames -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 500 }
            Note ('window {0} gpu-only held {1:0.0} s' -f $w, $sw.Elapsed.TotalSeconds)
        } else {
            $p = Start-Process -FilePath $perf -ArgumentList $pvArgs -PassThru -WindowStyle Hidden
            # The collector may use its own time plus 30 s, but never the cleanup reserve.
            $how = Wait-Exit $p ([Math]::Min($secs + 30, (Left) - $reserve))
            if ($how -eq 'exited') { Note "window $w collector exit $($p.ExitCode)" }
            else { Note "window $w collector over time or deadline: $how"; Latch $how $p 'PerfView collect' }
        }
    } catch {
        Note "window $w error: $($_.Exception.Message)"
    } finally {
        $closed = Stop-Owned $w $session
        if (!$closed) { $script:cleanupFailed = $true }
        Get-ChildItem -LiteralPath $Root -File | Where-Object { $_.Name -like "perf-$w*" -or $_.Name -like "gpu-$w*.etl" } |
            ForEach-Object { Note ("file {0} {1}" -f $_.Name, $_.Length) }
    }
}
function DwmPct([ref]$prev) {
    $d = Get-Process -Name dwm -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$d) { return -1 }
    $now = [DateTime]::UtcNow; $cpu = $d.TotalProcessorTime.TotalSeconds
    $r = -1
    if ($prev.Value -and $prev.Value[0] -eq $d.Id) {
        $r = 100 * ($cpu - $prev.Value[2]) / (($now - $prev.Value[1]).TotalSeconds * [Environment]::ProcessorCount)
    }
    $prev.Value = @($d.Id, $now, $cpu)
    return $r
}
function Finish { Note "etw-capture end cleanup_failed=$script:cleanupFailed"; exit ([int]$script:cleanupFailed) }

Note ("etw-capture begin tag=$Tag perfview=$((Get-FileHash -LiteralPath $perf).Hash.Substring(0,8)) not_after_qpc=$NotAfterQpc")
if ($Smoke) { $t0 = [DateTime]::UtcNow; Note 'smoke: one window, no game'; Capture 'S'; Finish }
$wait = [Diagnostics.Stopwatch]::StartNew()
while (!(Get-Process -Name $gameNames -ErrorAction SilentlyContinue)) {
    if ($wait.Elapsed.TotalSeconds -gt 90 -or (Left) -lt 60) { Note "no $Process in time, nothing captured"; Finish }
    Start-Sleep -Milliseconds 500
}
$t0 = [DateTime]::UtcNow
Note "$Process seen"
if ($SkipA) { Note 'window A skipped (-SkipA)' }
else {
    while (([DateTime]::UtcNow - $t0).TotalSeconds -lt $StartA) { Start-Sleep -Milliseconds 250 }
    Capture 'A'
}
if ($script:unjoined) { Note "window B skipped: unjoined pid $($script:unjoined)"; Finish }
# B's length: -SecondsB when given (cut to what the deadline leaves), else -Seconds; the smallest B worth starting.
$lenB = if ($SecondsB -gt 0) { $SecondsB } else { $Seconds }
$minB = if ($SecondsB -gt 0) { [Math]::Min(10, $lenB) } else { $Seconds }
function Start-B([string]$why) {
    $s = [int][Math]::Floor([Math]::Min($lenB, (Left) - $reserve - 1))
    Note "window B trigger $why, $s s"
    Capture 'B' $s
}
$prev = $null; $hits = 0
while ($true) {
    $el = ([DateTime]::UtcNow - $t0).TotalSeconds
    if ((Left) -lt $minB + $reserve + 1) { Note ("window B skipped: {0:0} s left at t={1:0}" -f (Left), $el); break }
    if (!(Get-Process -Name $gameNames -ErrorAction SilentlyContinue)) { Note 'window B skipped: game gone'; break }
    if ($WorldLog) {
        # game-runtime.ps1 keeps its log open for writing (FileShare.Read): read it with ReadWrite sharing.
        $world = $null
        try {
            $fs = New-Object IO.FileStream($WorldLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try { $text = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
            $m = [regex]::Match($text, 'walk [0-9]+s: start')
            if ($m.Success) { $world = $m.Value }
        } catch {}
        if ($world) { Start-B ('world: ' + $world); break }
    } else {
        $pct = DwmPct ([ref]$prev)
        if ($pct -ge $DwmPct) { $hits++ } else { $hits = 0 }
        if ($hits -ge 2) { Start-B ('dwm {0:0.0}%' -f $pct); break }
    }
    if ($el -ge $LatestB) { Start-B 'time'; break }
    Start-Sleep -Seconds 2
}
Finish
