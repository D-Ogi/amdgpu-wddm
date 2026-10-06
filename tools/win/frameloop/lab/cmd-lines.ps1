# The .cmd wrapper a frameloop run is started through, in one function so that the host can check it.
#
# Why a wrapper at all: a scheduled-task action cannot redirect output, and the client's text summary is worth
# keeping next to its JSON. Why a function: the first version of these lines was written inline and an operator
# precedence mistake ('a' + $x, 'b') put the log path on a line of its own; cmd then executed the path, Notepad
# opened on the lab's console desktop and the task sat there until its time limit killed it. Four lines, checked
# here and again on the lab before the task is registered.
#
# Dot-sourced by frameloop-task.ps1 (lab) and by host-checks.ps1 (this machine).

function New-FrameloopCmdLines {
    param([Parameter(Mandatory)][string]$Exe,
          [Parameter(Mandatory)][AllowEmptyString()][string]$ArgLine,
          [Parameter(Mandatory)][string]$Out,
          [Parameter(Mandatory)][string]$Log,
          # Driver knobs for this run only, as NAME=VALUE. Only BC250_* names are accepted: a plan sets a knob of
          # our own stack, never PATH or anything else cmd and the client read. One run of a set can therefore
          # carry BC250_SELF_WAIT=keep while the next carries elide, without a machine-wide cfg file that would
          # change the desktop's own ICD at the same time.
          [AllowEmptyCollection()][string[]]$Env = @())
    # Every path is quoted: C:\BC250 has no spaces today, but a quoted path costs nothing and an unquoted one
    # fails silently in cmd. %ERRORLEVEL% is the client's, and it leaves the wrapper as the task's result.
    #
    # The exit-code line puts its redirection first, which looks odd and is the only form that works: cmd reads
    # a digit immediately before >> as a file handle, so `echo exit %ERRORLEVEL%>> "log"` expands to
    # `echo exit 0>> "log"`, which echoes "exit " to the console and redirects stdin to the log. Found by
    # host-checks.ps1 after it had already happened on the lab.
    $lines = @('@echo off')
    foreach ($pair in $Env) {
        # -cnotmatch, not -notmatch: the default comparison ignores case, and a set file that says
        # bc250_self_wait should be corrected rather than quietly accepted next to the upper-case names.
        if ($pair -cnotmatch '^(BC250_[A-Z0-9_]+)=([^"%\r\n]*)$') { throw ('Not a BC250 knob: ' + $pair) }
        # set "NAME=VALUE": the quotes keep a trailing space out of the value, and cmd strips them itself.
        $lines += ('set "' + $Matches[1] + '=' + $Matches[2] + '"')
    }
    $lines += @(('"' + $Exe + '" ' + $ArgLine + ' --out "' + $Out + '" > "' + $Log + '" 2>&1'),
                ('>> "' + $Log + '" echo exit %ERRORLEVEL%'),
                'exit /b %ERRORLEVEL%')
    $lines
}

function Assert-FrameloopCmdLines {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines)
    # Four lines plus one per knob, and the knobs sit between @echo off and the client, where cmd reads them.
    if ($Lines.Count -lt 4) { throw ('The run wrapper must be at least 4 lines, not ' + $Lines.Count + ': ' + ($Lines -join ' | ')) }
    if ($Lines[0] -ne '@echo off') { throw 'The run wrapper must start with @echo off' }
    $i = 1
    while ($i -lt $Lines.Count -and $Lines[$i] -like 'set "*') {
        if ($Lines[$i] -cnotmatch '^set "BC250_[A-Z0-9_]+=[^"%]*"$') { throw ('Bad knob line: ' + $Lines[$i]) }
        $i++
    }
    if ($Lines.Count - $i -ne 3) { throw ('The run wrapper must end with the client, the exit code and exit /b: ' + ($Lines -join ' | ')) }
    if ($Lines[$i] -notmatch '^".+amdgpu_wddm_frameloop\.exe" .* --out ".+" > ".+" 2>&1$') { throw ('Bad client line: ' + $Lines[$i]) }
    if ($Lines[$i + 1] -notmatch '^>> ".+" echo exit %ERRORLEVEL%$') { throw ('Bad exit-code line: ' + $Lines[$i + 1]) }
    if ($Lines[$i + 2] -ne 'exit /b %ERRORLEVEL%') { throw 'The run wrapper must end with exit /b %ERRORLEVEL%' }
    foreach ($line in $Lines) {
        if ($line -match '^[A-Za-z]:\\' -or $line -eq '"') { throw ('A path or a quote on a line of its own: ' + $line) }
    }
}
