# Lab-side bounded ETW CPU profile of a game trial (owner request, review 848: use the collected profilers).
# Runs as a one-shot SYSTEM task started by etw-start.ps1 right after the trial's runtime starts. Two windows:
#   A: $StartA s after the game appears, $Seconds long (owner: DWM misbehaves from the game start). The game is
#      -Process (comma-separated Get-Process names, wildcards allowed; default witcher3, game runner profiles).
#   B: when dwm.exe CPU exceeds $DwmPct % of the machine in two consecutive 2 s samples, or at $LatestB s after
#      the game appeared, whichever first. From $LatestB on the window waits for the world band of
#      world-rule.ps1 when -WorldTelemetry names a sampler file (BD-107).
# One deadline bounds everything: $NotAfterQpc is the trial's own runtime cutoff (watch.ps1 origin + 255 s, less a
# margin, computed by etw-start.ps1 from the trial's start.json) on the shared QPC clock (review 854). A window is
# only started when its collection plus the cleanup reserve fits before it, and every wait, logman call and
# helper exit is cut to what is left; an exit that cannot be verified in time counts as a failed cleanup.
# Each window owns the sessions named BC250-<Tag>-<W>* (PerfView's own pair plus the DxgKrnl one); cleanup of
# owned sessions runs in finally, and a failed session query is never taken as absence (review 850). The script
# exits 1 when any cleanup failed; task exit alone is still not a closure witness: the operator's etw-closure.ps1,
# run once this task is no longer Running, is the independent receipt.
# FPS mode (owner, 2026-09-30: Full HD frame-rate tests): -GpuOnly records only the DxgKrnl session (no PerfView
# CPU profile, so the measurement does not load the CPU and stops in seconds); -SkipA drops window A; -WorldLog
# starts window B when the game log shows the walk phase ("walk Ns: start", the world is up) and B then lasts
# -SecondsB or what the deadline leaves, whichever is shorter.
# Present-mode mode (M15.14): -PresentMode adds Microsoft-Windows-Win32k and Microsoft-Windows-Dwm-Core to the
# DxgKrnl session, with narrow keyword masks, so that etw-present-mode.py can say per frame whether a present was
# scanned out or composed. It is off by default and changes nothing else about a window.
# World rule (BD-107): -WorldTelemetry <path> names the trial's DPM sampler file (C:\BC250\tmp\dpm-<trial>.txt)
# and world-rule.ps1 next to this script decides from it whether the GPU is in the game's world or in its menu.
# It gates the time-bounded fallback below, which in the b26 validation opened window B on the Witcher 3 main
# menu: from -LatestB on, the window waits for the world band instead of opening at once, and -WorldSettleSeconds
# says how long that band must hold. Without the switch, without the rule file, or with a sampler that stopped,
# the fallback opens as it always did and the note says the world was not verified, because a window opened on an
# unverified picture is still better than no window at all.
# Scheduler-stack mode (C49): -SchedulerStacks adds a second, stack-enabled DxgKrnl stream to PerfView's own user
# session, filtered to the scheduler's decision events, next to the cheaper kernel set that prices a DPC. It
# answers one question the packet-level session cannot: which dxgkrnl call site puts a context back into the node's
# ready set after a DMA completion, and whether that site runs in a DPC or on a thread. It needs the CPU window, so
# it is ignored with -GpuOnly, and it leaves the logman -gpu session exactly as it is, because that session is what
# c45b-window.py and c48gaps.py parse.
param([string]$Root, [string]$Tag, [int]$Seconds = 30, [int]$StartA = 5, [int]$DwmPct = 15, [int]$LatestB = 110,
    [long]$NotAfterQpc = 0, [switch]$Smoke, [switch]$GpuOnly, [switch]$SkipA, [string]$WorldLog = '', [int]$SecondsB = 0,
    [int]$ReserveSeconds = 0, [string]$Process = 'witcher3', [switch]$PresentMode, [switch]$SchedulerStacks,
    [string]$WorldTelemetry = '', [int]$WorldSettleSeconds = 25)
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
# it is noted with its PID, cleanup counts as failed, and no further helper is started (review 865).
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
# Every launch, direct ones included, records an exit it could not witness here (review 866).
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
    # -SchedulerStacks (C49): the same PerfView collection, with three changes and nothing else.
    #   1. No CPU sampling and no disk: /Profile and the DiskIO groups are the expensive part of /ThreadTime
    #      (measured 1.6 ms a frame on the W3 main thread, enough to move the rate by 10 %), and the question here
    #      is not where CPU time goes. /KernelEvents is given explicitly instead of /ThreadTime so that what is
    #      recorded is visible in this line: DPCs and interrupts (which routine ran, and for how long), context
    #      switches and the dispatcher's ready events (who woke whom), plus the process/thread/image rundown every
    #      stack walk needs to resolve a module.
    #   2. /StackCompression, because a stack-enabled per-event stream is mostly repeated frames.
    #   3. One DxgKrnl stream in PerfView's own user session, keyword mask 0x88008001 =
    #      Base | GPUScheduler | Present | Deprecated (read off the provider manifest, where UpdateContextStatus
    #      is id 20 under GPUScheduler 0x8000 and SelectContext2 is id 436 under Deprecated 0x80000000), level 5,
    #      and - the point of the mode - a STACK filter. The ids asked for are the scheduler's decisions (20
    #      UpdateContextStatus, 238 UnwaitQueuePacket, 436 SelectContext2) and the events that place them in time
    #      (175-177 DmaPacket, 181 VSyncInterrupt, 17 VSyncDPC, 18/19 WorkerThread, 22 AttemptPreemption), with
    #      stacks on the three decisions only. A stack on every DmaPacket would be the whole cost again.
    #
    #      Two things about that filter, both read off PerfView 3.2.8's own help text rather than assumed. It is
    #      ADDITIVE: "@EventIDsToEnable - a space separated list of decimal event ID numbers to collect ... in
    #      addition to any events specified by the Keywords", and TraceEvent's own summary says the same. So the
    #      keyword mask still decides the base volume (about 19 MB/s on RotTR D3D12) and this list only adds to it;
    #      what the mode narrows is the stack walking. And the lists are SPACE separated, while /Providers itself is
    #      "a comma separated list of specifications for providers" - a comma inside an id list would split the spec
    #      into further providers named "238", "436" and so on, and PerfView would then enable one event id and no
    #      stack filter at all, which is the one thing this mode exists for. The whole spec is therefore one quoted
    #      element: Start-Process -ArgumentList adds no quotes in either shell, so they are written here and Windows
    #      argv splitting hands PerfView one element with its spaces intact (dryrun-args.ps1 proves the round trip).
    #      The volume this costs is not claimed here: the -Smoke -SchedulerStacks run before the first game window
    #      reports the measured bytes and the stacked event count, and the plain -WorldSeconds control window prices
    #      the game-side cost.
    # The logman -gpu session below is untouched: it stays the unfiltered packet-level source of the gap table.
    if ($SchedulerStacks -and !$GpuOnly) {
        $ids = '20 238 436 175 176 177 181 17 18 19 22'
        $stackIds = '20 238 436'
        $spec = "/Providers:`"Microsoft-Windows-DxgKrnl:0x88008001:5:@EventIDsToEnable=$ids;@EventIDStacksToEnable=$stackIds`""
        $pvArgs = @("/LogFile:$log", '/NoGui', '/NoView', '/AcceptEULA', "/SessionName:$session",
            '/ClrEvents:None', '/StackCompression',
            '/KernelEvents:Process,Thread,ImageLoad,ContextSwitch,Dispatcher,DeferedProcedureCalls,Interrupt',
            $spec,
            '/BufferSizeMB:64', '/CircularMB:512', "/MaxCollectSec:$secs", '/NoClrRundown', '/NoNGenRundown',
            '/NoNGenPdbs', '/Merge:false', '/Zip:false', 'collect', $etl)
    }
    # GPU side (owner: what exactly does a frame wait for): DxgKrnl queue packets, DMA packets and VSync/flip in a
    # separate logman session, same shape as the G0 DWM trials (all keywords, level 5, 1 MB buffers, 64-256),
    # circular at 256 MB: up to 256 MB of buffers on top of PerfView's 32 MB request (recorded in review 850).
    $gpu = "$session-gpu"
    # 161 (3-minute FPS window): 159 wrote 2.4 MB/s, so a GPU-only window longer than 90 s gets 3 MB per second plus
    # 64 MB instead of the 256 MB circle, which would otherwise overwrite the window's start. 257 (LOW at 53 fps with
    # two presenting processes) wrote ~4.4 MB/s and lost its first 90 s to the 1054 MB circle: 6 MB per second now.
    # 397-399: RotTR D3D12 wrote ~19 MB/s and W3 LOW ~4.3 MB/s, so the fixed 256 MB circle of windows up to 90 s lost
    # their first 7-32 s (no event of any process there, read once as a game stall). Size every window for 20 MB/s,
    # capped at 4 GB (P: and the lab disk); the analysis reports the first present, so a wrapped circle still shows.
    $perSecond = 20
    # -SchedulerStacks does not change this number: it adds no provider to the logman session, and its own
    # stacked stream is sized by the /CircularMB above. The 40 MB/s the plan reserved for it covers both files
    # together (20 MB/s here plus the stacked stream, measured at the smoke run before the first game window).
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
        Note "window $w start session=$session seconds=$secs gpu_only=$([bool]$GpuOnly) scheduler_stacks=$(if ($SchedulerStacks -and $GpuOnly) { 'ignored-with-GpuOnly' } else { [bool]$SchedulerStacks })"
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
# The world rule (BD-107), from next to this script, because etw-start.ps1 copies both files into the trial's
# capture directory. The dot-source is HERE, at script scope, and not inside World-Now: a dot-source inside a
# function defines that script's functions in the function's own scope, and they are gone when it returns. Loaded
# lazily from inside World-Now, the first poll worked and every later one lost Test-WorldTelemetry, reported
# telemetry=$false and opened the window unverified - the defect of BD-107, two seconds late. A missing rule file
# is noted once by World-Now and then behaves like no telemetry.
$script:worldRulePath = Join-Path (Split-Path -Parent $PSCommandPath) 'world-rule.ps1'
$script:worldRule = [bool](Test-Path -LiteralPath $script:worldRulePath)
if ($script:worldRule) { . $script:worldRulePath }
$script:worldRuleNoted = $false
function World-Now {
    if (!$WorldTelemetry) { return @{ world = $false; telemetry = $false; why = 'no -WorldTelemetry' } }
    if (!$script:worldRule) {
        if (!$script:worldRuleNoted) {
            $script:worldRuleNoted = $true
            Note 'world rule not staged next to the capture; the time bound opens unverified'
        }
        return @{ world = $false; telemetry = $false; why = 'world-rule.ps1 not staged' }
    }
    try { return Test-WorldTelemetry -Path $WorldTelemetry -SettleSeconds $WorldSettleSeconds }
    catch { return @{ world = $false; telemetry = $false; why = "world rule error: $($_.Exception.Message)" } }
}

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
$prev = $null; $hits = 0; $script:timeHeld = $false
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
    # The time bound is no longer a trigger by itself (BD-107): from here on the window opens as soon as the
    # world band holds, and only a sampler that says nothing lets it open unverified.
    if ($el -ge $LatestB) {
        $w = World-Now
        if ($w.world) { Start-B ('time, world: ' + $w.why); break }
        if (!$w.telemetry) { Start-B ('time, world unverified: ' + $w.why); break }
        if (!$script:timeHeld) { $script:timeHeld = $true; Note ("window B held at the time bound t={0:0}: {1}" -f $el, $w.why) }
    }
    Start-Sleep -Seconds 2
}
Finish
