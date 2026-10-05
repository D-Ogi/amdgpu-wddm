# Offline self-test of the scripts in this directory. It needs no lab and no network.
# It makes no sound: alarm.ps1 runs only in a case that fails parameter binding before the player
# or the beep fallback runs.
# Usage: powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\selftest.ps1
# Exit codes: 0 all checks passed, 1 at least one check failed.
# 'Continue': a native command that writes to stderr must not stop this script.
$ErrorActionPreference = 'Continue'
$tools = $PSScriptRoot
$root = Join-Path $env:TEMP ('mon-selftest-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $root | Out-Null
$script:fail = 0
function Check($name, $cond, $detail) {
    if ($cond) { "PASS $name" } else { $script:fail++; "FAIL $name : $detail" }
}
function Start-Watch($argList) {
    Start-Job -ScriptBlock {
        param($a)
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $a[0] @($a[1..($a.Count - 1)])
    } -ArgumentList (, $argList)
}
function Finish($job) {
    $job | Wait-Job -Timeout 90 | Out-Null
    $lines = @(Receive-Job $job)
    Remove-Job $job -Force
    , $lines
}

# 1. watch-dir: a directory that does not exist -> exit 2
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$tools\watch-dir.ps1" -Path "$root\nope" 2>$null
Check 'watch-dir missing dir exit 2' ($LASTEXITCODE -eq 2) "exit $LASTEXITCODE"

$watched = Join-Path $root 'q'
New-Item -ItemType Directory -Path $watched | Out-Null
Set-Content -LiteralPath "$watched\a.md" -Value 'first'

# 2. watch-dir: nothing changes inside the deadline -> the no-change line
$out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$tools\watch-dir.ps1" -Path $watched -Filter *.md -Minutes 1 -IntervalSeconds 1 2>$null
Check 'watch-dir quiet run prints no-change' (($out -join '') -match 'no change in 1 min') "got '$out'"

# 3. watch-dir: a new file -> one labelled line, then exit
$job = Start-Watch @("$tools\watch-dir.ps1", '-Path', $watched, '-Filter', '*.md', '-Minutes', '1', '-IntervalSeconds', '1', '-ExitOnChange', '-Label', 'message')
Start-Sleep -Seconds 6
Set-Content -LiteralPath "$watched\b.md" -Value 'new file'
$lines = Finish $job
Check 'watch-dir reports a new file with the label' (($lines -join '|') -match 'message: b\.md') "got '$lines'"
Check 'watch-dir exits after the first line' ($lines.Count -eq 1) "got $($lines.Count) lines"

# 4. watch-dir: an edit counts as a change, and the filter keeps other files out
$job = Start-Watch @("$tools\watch-dir.ps1", '-Path', $watched, '-Filter', '*.md', '-Minutes', '1', '-IntervalSeconds', '1', '-ExitOnChange')
Start-Sleep -Seconds 6
Set-Content -LiteralPath "$watched\ignored.txt" -Value 'not matched'
Start-Sleep -Seconds 4
Set-Content -LiteralPath "$watched\a.md" -Value 'edited, and longer than before'
$lines = Finish $job
Check 'watch-dir reports an edit and skips the filtered file' (($lines -join '|') -eq 'changed: a.md') "got '$lines'"

# 5. watch-trial-log: keeps the key lines, drops the rest, stops at a closure marker
$log = Join-Path $root 'trial.log'
Set-Content -LiteralPath $log -Value @('> step one', 'noise line', 'status running')
$job = Start-Watch @("$tools\watch-trial-log.ps1", '-Log', $log, '-Minutes', '1', '-IntervalSeconds', '1')
Start-Sleep -Seconds 6
Add-Content -LiteralPath $log -Value @('more noise', '> step two', 'archive written')
$lines = Finish $job
Check 'watch-trial-log keeps the key lines' (($lines -join '|') -eq '> step one|status running|> step two|archive written') "got '$lines'"

# 6. watch-trial-log: a log that does not exist yet, and a line longer than -Width
$late = Join-Path $root 'late.log'
$job = Start-Watch @("$tools\watch-trial-log.ps1", '-Log', $late, '-Minutes', '1', '-IntervalSeconds', '1', '-Width', '10')
Start-Sleep -Seconds 6
Set-Content -LiteralPath $late -Value @(('> ' + ('x' * 300)), 'not closed here')
$lines = Finish $job
Check 'watch-trial-log waits for the log and cuts at -Width' (($lines -join '|') -eq '> xxxxxxxx|not closed') "got '$lines'"

# 7. alarm.ps1: the range check refuses 4 before the script body runs, so this makes no sound
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$tools\alarm.ps1" -Times 4 2>$null
Check 'alarm refuses -Times 4 (no sound played)' ($LASTEXITCODE -ne 0) "exit $LASTEXITCODE"

Remove-Item -LiteralPath $root -Recurse -Force
if ($script:fail -gt 0) { "$($script:fail) check(s) failed"; exit 1 } else { 'all checks passed'; exit 0 }
