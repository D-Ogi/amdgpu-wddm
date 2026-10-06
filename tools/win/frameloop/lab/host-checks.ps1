# Host checks for the lab runner's moving parts, on this machine, with no lab involved.
#
#   powershell -ExecutionPolicy Bypass -File lab\host-checks.ps1 [-Exe <client>]
#
# What it checks: the .cmd wrapper every run is started through is exactly the four lines it should be, cmd
# actually runs it (redirection, the exit code reaching the task), the task script parses, and the shapes that
# have already gone wrong once are refused. Exit 0 and a PASS line, or the first failure.
#
# The client binary lives in the workspace, not in the repository (src\build.ps1 writes it to
# <workspace>\scratch\m15\frameloop\build). The two checks that run it are skipped, not failed, when it is not
# built here: they are a gate on the wrapper, and a repository checkout alone cannot have the binary.
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

if (-not $Exe) {
    $root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else {
        $probe = $here
        while ($probe -and -not (Test-Path -LiteralPath (Join-Path $probe 'toolchain'))) { $probe = Split-Path -Parent $probe }
        $probe
    }
    $work = if ($env:BC250_FRAMELOOP_WORK) { $env:BC250_FRAMELOOP_WORK } elseif ($root) { Join-Path $root 'scratch\m15\frameloop' } else { '' }
    $Exe = if ($work) { Join-Path $work 'build\amdgpu_wddm_frameloop.exe' } else { 'amdgpu_wddm_frameloop.exe' }
}
$exe = $Exe
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
if ($skipped) { "HOST-CHECKS PASS $checks checks, $skipped skipped (no built client)"; exit 0 }
"HOST-CHECKS PASS $checks checks"
