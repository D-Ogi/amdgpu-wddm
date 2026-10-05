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
          [Parameter(Mandatory)][string]$Log)
    # Every path is quoted: C:\BC250 has no spaces today, but a quoted path costs nothing and an unquoted one
    # fails silently in cmd. %ERRORLEVEL% is the client's, and it leaves the wrapper as the task's result.
    #
    # The exit-code line puts its redirection first, which looks odd and is the only form that works: cmd reads
    # a digit immediately before >> as a file handle, so `echo exit %ERRORLEVEL%>> "log"` expands to
    # `echo exit 0>> "log"`, which echoes "exit " to the console and redirects stdin to the log. Found by
    # host-checks.ps1 after it had already happened on the lab.
    @('@echo off',
      ('"' + $Exe + '" ' + $ArgLine + ' --out "' + $Out + '" > "' + $Log + '" 2>&1'),
      ('>> "' + $Log + '" echo exit %ERRORLEVEL%'),
      'exit /b %ERRORLEVEL%')
}

function Assert-FrameloopCmdLines {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines)
    if ($Lines.Count -ne 4) { throw ('The run wrapper must be 4 lines, not ' + $Lines.Count + ': ' + ($Lines -join ' | ')) }
    if ($Lines[0] -ne '@echo off') { throw 'The run wrapper must start with @echo off' }
    if ($Lines[1] -notmatch '^".+amdgpu_wddm_frameloop\.exe" .* --out ".+" > ".+" 2>&1$') { throw ('Bad client line: ' + $Lines[1]) }
    if ($Lines[2] -notmatch '^>> ".+" echo exit %ERRORLEVEL%$') { throw ('Bad exit-code line: ' + $Lines[2]) }
    if ($Lines[3] -ne 'exit /b %ERRORLEVEL%') { throw 'The run wrapper must end with exit /b %ERRORLEVEL%' }
    foreach ($line in $Lines) {
        if ($line -match '^[A-Za-z]:\\' -or $line -eq '"') { throw ('A path or a quote on a line of its own: ' + $line) }
    }
}
