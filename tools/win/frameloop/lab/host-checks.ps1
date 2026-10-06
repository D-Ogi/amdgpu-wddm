# Host checks for the lab runner's moving parts, on this machine, with no lab involved.
#
#   powershell -ExecutionPolicy Bypass -File lab\host-checks.ps1 [-Exe <client>]
#
# What it checks: the .cmd wrapper every run is started through is exactly the four lines it should be, cmd
# actually runs it (redirection, the exit code reaching the task), the task script parses, and the shapes that
# have already gone wrong once are refused. Exit 0 and a PASS line, or the first failure.
#
# The client binary lives in the workspace, not in the repository (src\build.ps1 writes it to
# <workspace>\scratch\m15\frameloop\build). The checks that run it are skipped, not failed, when it is not
# built here: they are a gate on the wrapper, and a repository checkout alone cannot have the binary. The C48
# analysis script is in the workspace for the same reason, so its check is skipped the same way.
param([string]$Exe = '')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$here\cmd-lines.ps1"
$checks = 0
$failures = 0
$skipped = 0
function Check([string]$what, [scriptblock]$body) {
    $script:checks++
    try {
        & $body
        "ok    $what"
    } catch {
        $script:failures++
        "FAIL  $what : $($_.Exception.Message)"
    }
}
# A check that needs the built client: skipped with a line of its own where the binary is not there.
function CheckExe([string]$what, [scriptblock]$body) {
    if (-not (Test-Path -LiteralPath $script:exe)) {
        $script:skipped++
        "skip  $what (no client at $script:exe)"
        return
    }
    Check $what $body
}
# A check that needs a file of the workspace: skipped the same way.
function CheckFile([string]$what, [string]$path, [scriptblock]$body) {
    if (-not $path -or -not (Test-Path -LiteralPath $path)) {
        $script:skipped++
        "skip  $what (not in this checkout: $path)"
        return
    }
    Check $what $body
}

# The workspace root is the first directory above this one that holds the toolchain.
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else {
    $probe = $here
    while ($probe -and -not (Test-Path -LiteralPath (Join-Path $probe 'toolchain'))) { $probe = Split-Path -Parent $probe }
    $probe
}
$work = if ($env:BC250_FRAMELOOP_WORK) { $env:BC250_FRAMELOOP_WORK } elseif ($root) { Join-Path $root 'scratch\m15\frameloop' } else { '' }
if (-not $Exe) {
    $Exe = if ($work) { Join-Path $work 'build\amdgpu_wddm_frameloop.exe' } else { 'amdgpu_wddm_frameloop.exe' }
}
$exe = $Exe
# The C48 repro analyser is analysis scaffolding of one ETW session, so it stays in the workspace.
$c48repro = if ($root) { Join-Path $root 'scratch\m15\etw\c48\c48repro.py' } else { '' }
$tmp = Join-Path $env:TEMP ('frameloop-host-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$null = New-Item -ItemType Directory -Path $tmp

try {
    Check 'the wrapper is exactly four lines' {
        $lines = New-FrameloopCmdLines -Exe 'C:\BC250\frameloop\amdgpu_wddm_frameloop.exe' `
            -ArgLine '--seconds 10 --gpu-ms 12 --lists 7' -Out 'C:\BC250\frameloop\runs\t\a.json' `
            -Log 'C:\BC250\frameloop\runs\t\a.txt'
        if ($lines.Count -ne 4) { throw "got $($lines.Count)" }
        Assert-FrameloopCmdLines -Lines $lines
    }
    Check 'a path on a line of its own is refused' {
        $bad = @('@echo off', '"C:\x\amdgpu_wddm_frameloop.exe" --out "a" > "', 'C:\BC250\runs\a.txt', '"', 'exit /b %ERRORLEVEL%')
        $caught = $false
        try { Assert-FrameloopCmdLines -Lines $bad } catch { $caught = $true }
        if (-not $caught) { throw 'accepted the shape that opened Notepad on the lab' }
    }
    Check 'an empty argument line still produces four sound lines' {
        Assert-FrameloopCmdLines -Lines (New-FrameloopCmdLines -Exe 'C:\a\amdgpu_wddm_frameloop.exe' -ArgLine '' -Out 'C:\a\b.json' -Log 'C:\a\b.txt')
    }
    Check 'paths with spaces stay quoted' {
        $lines = New-FrameloopCmdLines -Exe 'C:\Program Files\amdgpu_wddm_frameloop.exe' -ArgLine '--seconds 1' `
            -Out 'C:\out dir\a.json' -Log 'C:\out dir\a.txt'
        Assert-FrameloopCmdLines -Lines $lines
        if ($lines[1] -notmatch '^"C:\\Program Files\\amdgpu_wddm_frameloop\.exe" ') { throw 'the exe lost its quotes' }
    }
    Check 'the knobs of a run sit between @echo off and the client, one per line' {
        $lines = New-FrameloopCmdLines -Exe 'C:\BC250\frameloop\amdgpu_wddm_frameloop.exe' -ArgLine '--seconds 20' `
            -Out 'C:\BC250\frameloop\runs\t\a.json' -Log 'C:\BC250\frameloop\runs\t\a.txt' `
            -Env @('BC250_SELF_WAIT=elide', 'BC250_DEFERRED_SUMMARY_S=5')
        if ($lines.Count -ne 6) { throw "got $($lines.Count) lines" }
        if ($lines[1] -ne 'set "BC250_SELF_WAIT=elide"') { throw "first knob line is '$($lines[1])'" }
        if ($lines[2] -ne 'set "BC250_DEFERRED_SUMMARY_S=5"') { throw "second knob line is '$($lines[2])'" }
        Assert-FrameloopCmdLines -Lines $lines
    }
    Check 'a knob that is not ours, or that could escape its quotes, is refused' {
        foreach ($bad in @('PATH=C:\x', 'BC250_SELF_WAIT=a"b', 'BC250_SELF_WAIT=%PATH%', 'bc250_self_wait=elide', 'BC250_SELF_WAIT')) {
            $caught = $false
            try {
                $null = New-FrameloopCmdLines -Exe 'C:\a\amdgpu_wddm_frameloop.exe' -ArgLine '' -Out 'C:\a\b.json' `
                    -Log 'C:\a\b.txt' -Env @($bad)
            } catch { $caught = $true }
            if (-not $caught) { throw "the generator accepted $bad" }
        }
        # And a hand-made wrapper that sets something else is refused on the way in, where the lab checks it.
        $caught = $false
        try {
            Assert-FrameloopCmdLines -Lines @('@echo off', 'set "PATH=C:\x"',
                '"C:\a\amdgpu_wddm_frameloop.exe"  --out "a.json" > "a.txt" 2>&1',
                '>> "a.txt" echo exit %ERRORLEVEL%', 'exit /b %ERRORLEVEL%')
        } catch { $caught = $true }
        if (-not $caught) { throw 'Assert accepted a wrapper that sets PATH' }
    }
    Check 'cmd hands the knob to the process with the exact value' {
        # The quoted set form is the part that can go wrong silently: an unquoted one keeps a trailing space, and
        # a value nobody checked could close its own quote. Brackets around the echo make a stray space visible.
        $log = Join-Path $tmp 'knob.txt'
        $cmdFile = Join-Path $tmp 'knob.cmd'
        $lines = New-FrameloopCmdLines -Exe $exe -ArgLine '--help' -Out (Join-Path $tmp 'knob.json') -Log $log `
            -Env @('BC250_SELF_WAIT=elide')
        Assert-FrameloopCmdLines -Lines $lines
        # Keep the knob lines, replace the client with an echo of what cmd made of them.
        $probe = @($lines[0], $lines[1], ('> "' + $log + '" echo [%BC250_SELF_WAIT%]'), 'exit /b 0')
        Set-Content -LiteralPath $cmdFile -Value $probe -Encoding ASCII
        & cmd.exe /c "`"$cmdFile`""
        if ($LASTEXITCODE -ne 0) { throw "the probe exited $LASTEXITCODE" }
        $text = (Get-Content -LiteralPath $log -Raw).Trim()
        if ($text -ne '[elide]') { throw "cmd made the value '$text'" }
    }
    Check 'the lab side passes every run its own knobs and records them' {
        $text = Get-Content -LiteralPath (Join-Path $here 'frameloop-task.ps1') -Raw
        foreach ($needle in @('-Env $runEnv', 'env = $runEnv', 'BC250_DEFERRED_LOG=')) {
            if ($text -notlike "*$needle*") { throw "frameloop-task.ps1 no longer has: $needle" }
        }
    }
    CheckExe 'cmd runs the wrapper: redirection, log content and exit code' {
        $log = Join-Path $tmp 'selftest.txt'
        $cmdFile = Join-Path $tmp 'selftest.cmd'
        # --selftest needs no device and opens no window, so this is the one client run that is safe to make
        # part of a gate on the development PC.
        $lines = New-FrameloopCmdLines -Exe $exe -ArgLine '--selftest' -Out (Join-Path $tmp 'selftest.json') -Log $log
        Assert-FrameloopCmdLines -Lines $lines
        Set-Content -LiteralPath $cmdFile -Value $lines -Encoding ASCII
        & cmd.exe /c "`"$cmdFile`""
        $code = $LASTEXITCODE
        if ($code -ne 0) { throw "the wrapper exited $code" }
        $text = Get-Content -LiteralPath $log
        if (($text -join "`n") -notmatch 'SELFTEST PASS') { throw 'the client output did not reach the log' }
        if ($text[-1] -ne 'exit 0') { throw "the last log line is '$($text[-1])', not 'exit 0'" }
        if (Get-Process -Name notepad -ErrorAction SilentlyContinue) { throw 'something opened Notepad' }
    }
    CheckExe 'a failing client comes back as the wrapper''s exit code' {
        $log = Join-Path $tmp 'bad.txt'
        $cmdFile = Join-Path $tmp 'bad.cmd'
        $lines = New-FrameloopCmdLines -Exe $exe -ArgLine '--nonsense' -Out (Join-Path $tmp 'bad.json') -Log $log
        Set-Content -LiteralPath $cmdFile -Value $lines -Encoding ASCII
        & cmd.exe /c "`"$cmdFile`""
        if ($LASTEXITCODE -ne 2) { throw "the wrapper exited $LASTEXITCODE, not the client's 2" }
        $text = Get-Content -LiteralPath $log
        if ($text[-1] -ne 'exit 2') { throw "the last log line is '$($text[-1])', not 'exit 2'" }
    }
    Check 'the lab task script parses' {
        $errors = $null
        $null = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $here 'frameloop-task.ps1'), [ref]$null, [ref]$errors)
        if ($errors) { throw ($errors[0].Message + ' at line ' + $errors[0].Extent.StartLineNumber) }
    }
    Check 'the four timeouts are still in the right order' {
        # The one that already cost a trial: the task limit was below the client's own watchdog, so a run that
        # overran ended as SCHED_S_TASK_TERMINATED with nothing to read instead of with the client's own JSON.
        # The order must be soft stop < hard kill < task limit < poll deadline, and it lives in two files.
        $client = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $here) 'src\main.cpp') -Raw
        if ($client -notmatch 'g_watchdog_seconds \+ (\d+)') { throw 'no watchdog limit in main.cpp' }
        $soft = [int]$Matches[1]
        if ($client -notmatch 'const DWORD hard = (\d+)000;') { throw 'no watchdog hard window in main.cpp' }
        $hard = $soft + [int]$Matches[1]
        $taskText = Get-Content -LiteralPath (Join-Path $here 'frameloop-task.ps1') -Raw
        if ($taskText -notmatch 'ExecutionTimeLimit \(New-TimeSpan -Seconds \(\[int\]\$run\.seconds \+ (\d+)\)\)') { throw 'no task time limit' }
        $limit = [int]$Matches[1]
        if ($taskText -notmatch '\$runDeadline = \$begin\.AddSeconds\(\[int\]\$run\.seconds \+ (\d+)\)') { throw 'no poll deadline' }
        $poll = [int]$Matches[1]
        $runner = Get-Content -LiteralPath (Join-Path $here 'run-frameloop.py') -Raw
        if ($runner -notmatch 'RUN_WORST_CASE_EXTRA = (\d+)') { throw 'no worst case in run-frameloop.py' }
        $worst = [int]$Matches[1]
        if (-not ($soft -lt $hard -and $hard -lt $limit -and $limit -lt $poll)) {
            throw "order is soft $soft, hard $hard, task limit $limit, poll $poll"
        }
        if ($worst -ne $poll) { throw "the budget assumes $worst s of slack, the poll gives up at $poll s" }
    }
    Check 'the lab side reads the budget fields the runner writes' {
        $runner = Get-Content -LiteralPath (Join-Path $here 'run-frameloop.py') -Raw
        $taskText = Get-Content -LiteralPath (Join-Path $here 'frameloop-task.ps1') -Raw
        foreach ($field in @('deadline_seconds', 'run_worst_case_extra_seconds')) {
            if ($runner -notlike "*`"$field`"*") { throw "run-frameloop.py does not write $field" }
            if ($taskText -notlike "*$field*") { throw "frameloop-task.ps1 does not read $field" }
        }
    }
    Check 'the lab side records the operating point of every run, not of the set' {
        # K178: every game rate of sessions 347-403 was silently a 24 CU number, because nothing in the
        # evidence said which. Since DPM shipped the clock moves with the temperature as well, so two arms
        # run back to back are two operating points unless the record says otherwise.
        $text = Get-Content -LiteralPath (Join-Path $here 'frameloop-task.ps1') -Raw
        foreach ($needle in @('Read-OperatingPoint', 'point_before = Read-OperatingPoint',
                              '$record.point_after = Read-OperatingPoint', 'cu_record = $cuRecord',
                              'clock read')) {
            if ($text -notlike "*$needle*") { throw "frameloop-task.ps1 no longer has: $needle" }
        }
        $runner = Get-Content -LiteralPath (Join-Path $here 'run-frameloop.py') -Raw
        foreach ($needle in @('cu_record', 'point_before', 'clock_reader')) {
            if ($runner -notlike "*$needle*") { throw "run-frameloop.py does not read $needle" }
        }
    }
    Check 'the self-wait set has both keep arms, the count control and a written decision rule' {
        $sets = Get-Content -LiteralPath (Join-Path $here 'sets.json') -Raw | ConvertFrom-Json
        $arms = @($sets.sets.'self-wait'.runs | ForEach-Object { $_.name })
        foreach ($need in @('keep', 'count', 'elide', 'keep2')) {
            if ($arms -notcontains $need) { throw "the self-wait set has no $need arm: $($arms -join ', ')" }
        }
        if ([array]::IndexOf($arms, 'keep') -gt [array]::IndexOf($arms, 'elide')) {
            throw 'the first keep arm must run before elide'
        }
        if ([array]::IndexOf($arms, 'keep2') -lt [array]::IndexOf($arms, 'elide')) {
            throw 'keep2 must run after elide, or it shows no drift'
        }
        $what = [string]$sets.sets.'self-wait'.what
        foreach ($needle in @('322', 'K142', 'DECISION RULE')) {
            if ($what -notlike "*$needle*") { throw "the set does not state: $needle" }
        }
    }
    Check 'the runner withholds the comparison when an arm does not witness itself' {
        # Session 322 passed every mechanism witness and lost 1.7 % of the frame rate. A comparison printed
        # next to a failed witness gets quoted as a result, so the runner must refuse to print one.
        $dir = Join-Path $tmp 'witness'
        $null = New-Item -ItemType Directory -Path $dir
        # config.frame_statistics belongs to every arm here: the grid refusals are about an arm that ASKED for
        # the grid, and an arm without the flag (every set's smoke run) is not a refusal any more.
        $asked = @{ frame_statistics = $true }
        $good = @{ config = $asked
                   icd = @{ read = 'ok'; submit_line = 'x submit: self_wait=keep'; self_wait = 'keep'
                            wait_self_seen = 0; wait_self_dropped = 0; wait_self_busy = 0 }
                   summary = @{ gaps = @{ vblank = @{ fitted = $true }; aligned_share_usable = $true
                                          frame_statistics_calls = 100; frame_statistics_failures = 1 } } }
        $bad = @{ config = $asked
                  icd = @{ read = 'ok'; submit_line = 'x submit: self_wait=elide'; self_wait = 'elide'
                           wait_self_seen = 0; wait_self_dropped = 0; wait_self_busy = 3 }
                  summary = @{ gaps = @{ vblank = @{ fitted = $false }; aligned_share_usable = $false
                                         frame_statistics_calls = 100; frame_statistics_failures = 7 } } }
        $unread = @{ config = $asked
                     icd = @{ read = 'open failed: errno 13, Win32 32'; submit_line = ''; self_wait = '' }
                     summary = @{ gaps = @{ vblank = @{ fitted = $true }; aligned_share_usable = $true
                                            frame_statistics_calls = 100; frame_statistics_failures = 0 } } }
        ($good | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath (Join-Path $dir 'keep.json') -Encoding UTF8
        ($bad | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath (Join-Path $dir 'elide.json') -Encoding UTF8
        ($unread | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath (Join-Path $dir 'count.json') -Encoding UTF8
        $loader = Join-Path $tmp 'witness.py'
        @('import importlib.util, pathlib, sys',
          'spec = importlib.util.spec_from_file_location("rf", sys.argv[1])',
          'module = importlib.util.module_from_spec(spec)',
          'spec.loader.exec_module(module)',
          'module.witness(pathlib.Path(sys.argv[2]), sys.argv[3].split(","))') |
            Set-Content -LiteralPath $loader -Encoding ASCII
        $out = & python $loader (Join-Path $here 'run-frameloop.py') $dir 'keep,elide,count' 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "the witness loader exited $LASTEXITCODE : $out" }
        if ($out -notmatch 'COMPARISON WITHHELD') { throw "no refusal in: $out" }
        foreach ($reason in @('dropped no wait', 'record in use', 'GetFrameStatistics', 'no vblank grid',
                              'not read back')) {
            if ($out -notmatch [regex]::Escape($reason)) { throw "the refusal does not name '$reason': $out" }
        }
        if ($out -notmatch 'arm keep') { throw "the witness line of the keep arm is missing: $out" }
    }
    Check 'a trial whose arms all witness themselves is not withheld' {
        $dir = Join-Path $tmp 'witness-ok'
        $null = New-Item -ItemType Directory -Path $dir
        $summary = @{ gaps = @{ vblank = @{ fitted = $true }; aligned_share_usable = $true
                                frame_statistics_calls = 100; frame_statistics_failures = 1 } }
        $asked = @{ frame_statistics = $true }
        @{ config = $asked
           icd = @{ read = 'ok'; submit_line = 'x submit: self_wait=keep'; self_wait = 'keep'
                    wait_self_seen = 0; wait_self_dropped = 0; wait_self_busy = 0 }
           summary = $summary } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'keep.json') -Encoding UTF8
        @{ config = $asked
           icd = @{ read = 'ok'; submit_line = 'x submit: self_wait=count'; self_wait = 'count'
                    wait_self_seen = 12; wait_self_dropped = 0; wait_self_busy = 0 }
           summary = $summary } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'count.json') -Encoding UTF8
        # The elide arm of a client from before the _fsopen fix: an empty icd block, and the pulled ICD log of
        # the archive next to it, which the runner reads instead.
        @{ config = $asked
           icd = @{ submit_line = ''; self_wait = '' }
           summary = $summary } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'elide.json') -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $dir 'elide-icd.log') -Encoding ASCII -Value @(
            't=5s submit: submits=10 self_wait=elide wait_self_seen=12 wait_self_dropped=12 wait_self_busy=0')
        $out = & python (Join-Path $tmp 'witness.py') (Join-Path $here 'run-frameloop.py') $dir 'keep,count,elide' 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "the witness loader exited $LASTEXITCODE : $out" }
        if ($out -match 'COMPARISON WITHHELD') { throw "a sound trial was withheld: $out" }
        if ($out -notmatch 'every arm says what it is') { throw "no verdict line in: $out" }
    }
    Check 'the runner withholds the comparison when a cross-queue arm did not cross' {
        # The same rule as the ICD counters, for the queues: a handoff whose fence never moved did not run, and
        # a control whose fence moved is not a control. Without this, an arm that quietly fell back to one
        # queue would be compared as if it had two.
        $dir = Join-Path $tmp 'witness-handoff'
        $null = New-Item -ItemType Directory -Path $dir
        $icd = @{ read = 'ok'; submit_line = 'x submit: self_wait=keep'; self_wait = 'keep'
                  wait_self_seen = 0; wait_self_dropped = 0; wait_self_busy = 0 }
        $gaps = @{ vblank = @{ fitted = $true }; aligned_share_usable = $true
                   frame_statistics_calls = 100; frame_statistics_failures = 1 }
        $arm = {
            param($mode, $queues, $signals, $waits, $signalPerList, $clock)
            if (-not $clock) { $clock = if ($queues -gt 1) { 'one domain' } else { 'one queue' } }
            @{ config = @{ frame_statistics = $true }
               icd = $icd
               queue_handoff = @{ queues_created = $queues; handoff = $mode; signal_per_list = $signalPerList
                                  fence_signals = $signals; fence_waits = $waits; last_queue = '0'
                                  queue_clock_witness = $clock }
               summary = @{ frames_measured = 700; gaps = $gaps } }
        }
        (& $arm 'per-list' 2 5600 4800 $false) | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $dir 'd-2q-per-list.json') -Encoding UTF8
        (& $arm 'per-list' 2 5600 0 $false) | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $dir 'd-stuck.json') -Encoding UTF8
        (& $arm 'none' 1 40 0 $false) | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $dir 'a-noisy.json') -Encoding UTF8
        # A second queue that crossed, on a clock the client could not reconcile with the first: every
        # cross-queue duration in such a trial is an unknown offset, so the arm is not readable either.
        (& $arm 'per-list' 2 5600 4800 $false 'clock calibrations disagree') | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $dir 'd-two-clocks.json') -Encoding UTF8
        $out = & python (Join-Path $tmp 'witness.py') (Join-Path $here 'run-frameloop.py') $dir 'd-2q-per-list,d-stuck,a-noisy,d-two-clocks' 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "the witness loader exited $LASTEXITCODE : $out" }
        if ($out -notmatch 'COMPARISON WITHHELD') { throw "no refusal in: $out" }
        foreach ($reason in @('the per-list handoff did not cross', 'moved the handoff fence',
                              "clock witness says 'clock calibrations disagree'")) {
            if ($out -notmatch [regex]::Escape($reason)) { throw "the refusal does not name '$reason': $out" }
        }
        if ($out -notmatch 'handoff per-list') { throw "no witness line for the crossing arm: $out" }
        # And the crossing arm alone is not withheld.
        $ok = Join-Path $tmp 'witness-handoff-ok'
        $null = New-Item -ItemType Directory -Path $ok
        (& $arm 'per-list' 2 5600 4800 $false) | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $ok 'd-2q-per-list.json') -Encoding UTF8
        (& $arm 'none' 1 0 0 $false) | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $ok 'a-base.json') -Encoding UTF8
        (& $arm 'per-frame' 2 5600 740 $false) | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $ok 'f-2q-per-frame.json') -Encoding UTF8
        $out2 = & python (Join-Path $tmp 'witness.py') (Join-Path $here 'run-frameloop.py') $ok 'a-base,d-2q-per-list,f-2q-per-frame' 2>&1 | Out-String
        if ($out2 -match 'COMPARISON WITHHELD') { throw "sound cross-queue arms were withheld: $out2" }
    }
    Check 'the c48-handoff set has its controls, its witness rule and a written decision rule' {
        $sets = Get-Content -LiteralPath (Join-Path $here 'sets.json') -Raw | ConvertFrom-Json
        $set = $sets.sets.'c48-handoff'
        if (-not $set) { throw 'sets.json has no c48-handoff set' }
        $arms = @($set.runs | ForEach-Object { $_.name })
        foreach ($need in @('a-base', 'c-2q-none', 'd-2q-per-list', 'h-base-vsync1',
                            'e-2q-per-list-vsync1', 'f-2q-per-frame', 'g-base2')) {
            if ($arms -notcontains $need) { throw "the c48-handoff set has no $need arm: $($arms -join ', ')" }
        }
        # The one-queue control runs first and again last: without the repeat, a thermal drift over the set
        # reads as an effect of the arm that happened to run last (the self-wait set learnt this the hard way).
        if ([array]::IndexOf($arms, 'a-base') -gt [array]::IndexOf($arms, 'd-2q-per-list')) {
            throw 'the one-queue control must run before the handoff arms'
        }
        if ([array]::IndexOf($arms, 'g-base2') -ne $arms.Count - 1) { throw 'g-base2 must be the last arm' }
        if ([array]::IndexOf($arms, 'h-base-vsync1') -gt [array]::IndexOf($arms, 'e-2q-per-list-vsync1')) {
            throw 'the interval-1 control must run before the interval-1 handoff arm'
        }
        $what = [string]$set.what
        foreach ($needle in @('DECISION RULE', '418', '1.8 %', 'per_second_ge_4ms', 'aligned_share_ge_4ms',
                              'count_ge_4ms is at least 20', 'OWN present interval', 'h-base-vsync1',
                              'ACCESS_DENIED')) {
            if ($what -notlike "*$needle*") { throw "the set does not state: $needle" }
        }
        # Every present interval that carries a cross-queue arm needs a one-queue arm at the SAME interval.
        # Interval 1 paces the queue by itself: at --gpu-ms 12 against a 16.68 ms refresh it leaves about 5 ms
        # of node-0 idle a frame that ends just after the flip, which is the class's own shape. Without the
        # matched control, that pacing would be read as an effect of the handoff (this is why h exists).
        $interval = {
            param($line)
            if ($line -match '--present-interval\s+(\d+)') { [int]$matches[1] } else { 0 }
        }
        $byInterval = @{}
        foreach ($run in $set.runs) {
            if ($run.name -eq 'smoke') { continue }
            $line = @($run.args) -join ' '
            $iv = & $interval $line
            if (-not $byInterval.ContainsKey($iv)) { $byInterval[$iv] = @{ queues = @(); one = @() } }
            if ($line -match '--queues\s+2') { $byInterval[$iv].queues += $run.name }
            else { $byInterval[$iv].one += $run.name }
        }
        foreach ($iv in $byInterval.Keys) {
            if (@($byInterval[$iv].queues).Count -gt 0 -and @($byInterval[$iv].one).Count -eq 0) {
                throw ("present interval $iv has cross-queue arms ($($byInterval[$iv].queues -join ', ')) and" +
                       ' no one-queue control at the same interval')
            }
        }
        foreach ($run in $set.runs) {
            $line = @($run.args) -join ' '
            # Every measured arm needs the vblank grid: without --frame-statistics every aligned share is null
            # and the decision rule cannot be applied at all.
            if ($run.name -ne 'smoke' -and $line -notlike '*--frame-statistics*') {
                throw "arm $($run.name) has no --frame-statistics, so it could not be read"
            }
            if ($line -like '*--handoff*' -and $line -like '*--submit batch*') {
                throw "arm $($run.name) asks for a handoff with one call a frame, which the client refuses"
            }
        }
    }
    CheckExe 'the client refuses the handoff shapes the set must never contain' {
        # Argument parsing happens before any device or window, so these runs cost nothing and open nothing.
        $cases = @(
            @{ Args = @('--handoff', 'per-list');                              Why = 'a handoff without a second queue' },
            @{ Args = @('--queues', '2', '--handoff', 'per-list', '--submit', 'batch'); Why = 'a handoff in one call a frame' },
            @{ Args = @('--queues', '2', '--handoff', 'per-list', '--lists', '2');      Why = 'a per-list handoff over two lists' },
            @{ Args = @('--queues', '3');                                      Why = 'a third queue' }
        )
        # The client prints its usage on stderr when it refuses a command line, and under
        # $ErrorActionPreference = 'Stop' a native command's stderr line is itself a terminating error in
        # PowerShell 5.1. The exit code is the thing being checked here, so the preference is lowered for
        # these four runs only.
        $ErrorActionPreference = 'Continue'
        foreach ($case in $cases) {
            & $exe @($case.Args) 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 2) {
                throw "$($case.Why) was not refused (exit $LASTEXITCODE)"
            }
        }
    }
    Check 'the client, the runner and the analysis script name the same gap fields' {
        # The arms are read by name from the JSON. A field renamed in one place and not the other is how a
        # trial comes back unreadable, so the three sides are checked against each other here, on the host.
        $source = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $here) 'src\main.cpp') -Raw
        # The names as they appear in the C format strings, where every quote is escaped: a trailing quote in
        # the needle would never match and the check would always fail.
        foreach ($needle in @('count_ge_4ms', 'vblank_aligned_ge_4ms', 'aligned_share_ge_4ms',
                              'per_second_ge_4ms', 'queue_handoff', 'fence_signals', 'fence_waits',
                              'queue_clock_witness')) {
            if ($source -notlike "*\`"$needle\`"*") { throw "main.cpp no longer writes $needle" }
        }
        # The runner refuses a cross-queue arm whose queues do not share a timestamp domain, so it has to read
        # the field the client writes for it.
        $runnerText = Get-Content -LiteralPath (Join-Path $here 'run-frameloop.py') -Raw
        if ($runnerText -notlike '*queue_clock_witness*') { throw 'run-frameloop.py does not read queue_clock_witness' }
    }
    CheckFile 'the C48 analyser reads the counters the client writes' $c48repro {
        $text = Get-Content -LiteralPath $c48repro -Raw
        foreach ($needle in @('count_ge_4ms', 'per_second_ge_4ms', 'aligned_share_ge_4ms', 'queue_handoff',
                              'fence_signals', 'fence_waits', 'queue_clock_witness')) {
            if ($text -notlike "*$needle*") { throw "c48repro.py does not read $needle" }
        }
    }
    Check 'the task script uses the generator and checks what it wrote' {
        $text = Get-Content -LiteralPath (Join-Path $here 'frameloop-task.ps1') -Raw
        foreach ($needle in @('. "$PSScriptRoot\cmd-lines.ps1"', 'New-FrameloopCmdLines', 'Assert-FrameloopCmdLines -Lines @(Get-Content')) {
            if ($text -notlike "*$needle*") { throw "frameloop-task.ps1 no longer has: $needle" }
        }
    }
} finally {
    Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
}
if ($failures) { "HOST-CHECKS FAIL $failures of $checks"; exit 1 }
if ($skipped) { "HOST-CHECKS PASS $checks checks, $skipped skipped (not in this checkout)"; exit 0 }
"HOST-CHECKS PASS $checks checks"
