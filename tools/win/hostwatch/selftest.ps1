# Offline gate of the host watch and the kill switch. It touches no kd.exe and kills nothing but its own children.
#   pwsh -NoProfile -File tools\win\hostwatch\selftest.ps1 [-Out <dir>]
# Four live checks, preceded by deterministic file-sharing and partial-record controls:
#   parse        all three scripts parse with the Windows PowerShell 5.1 parser (no execution)
#   trigger      killswitch.ps1 with an impossible limit and a process name that cannot exist: the trigger fires,
#                the dry run kills nothing, and a stale stop file is cleared at start instead of ending the run
#   stop         the run ends after the stop file appears again
#   sample       hostwatch.ps1 writes nonpaged-pool lines at the interval it was given
# The live validation of the real kill path is killswitch-dryrun.ps1, which is run by hand because it kills every
# ping.exe on this PC.
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
param(
    [string]$Out = (Join-Path (& { if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path } }) 'scratch\hostwatch\selftest')
)
$ErrorActionPreference = 'Stop'
$failed = @()
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { "PASS $name" } else { $script:failed += $name; "FAIL $name - $detail" }
}


# Polling must coexist with the short Add-Content write handle of the PS 5.1 child.
# Sharing/lock violations consume the current poll, not a new timeout. Other IO errors stay fatal.
function Read-SharedLog([string]$Path) {
    $stream = $null; $reader = $null
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
        $text = $reader.ReadToEnd()
        # Add-Content terminates each record with a newline. A concurrent append may expose only its prefix.
        $last = $text.LastIndexOf("`n")
        if ($last -lt 0) { return '' }
        return $text.Substring(0, $last + 1)
    } catch [IO.FileNotFoundException] {
        return '' # The child has not written its first line yet.
    } catch [IO.IOException] {
        $code = $_.Exception.GetBaseException().HResult -band 0xffff
        if ($code -eq 32 -or $code -eq 33) { return '' }
        throw
    } finally {
        if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() }
    }
}
function Stop-SelftestChild($Child) {
    try {
        if (-not $Child.HasExited) {
            try { $Child.Kill() } catch [InvalidOperationException] { if (-not $Child.HasExited) { throw } }
        }
        if (-not $Child.WaitForExit(5000)) { throw 'Owned selftest child did not exit after cleanup.' }
    } finally { $Child.Dispose() }
}

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -File -ErrorAction SilentlyContinue | Remove-Item -Force
$ps51 = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$scripts = 'hostwatch.ps1', 'killswitch.ps1', 'killswitch-dryrun.ps1', 'selftest.ps1'

$errors = @()
foreach ($name in $scripts) {
    $tokens = $null; $parse = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$parse)
    if ($parse.Count) { $errors += "$name line $($parse[0].Extent.StartLineNumber): $($parse[0].Message)" }
}
Check 'parse' ($errors.Count -eq 0) ($errors -join '; ')


# Deterministic reader contract before any watcher child starts. This file belongs only to this test.
$sharingLog = Join-Path $Out 'reader-sharing.log'
$writer = $null
try {
    $complete = "reader complete`r`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($complete)
    $writer = [IO.File]::Open($sharingLog, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $writer.Write($bytes, 0, $bytes.Length); $writer.Flush()
    Check 'reader-exclusive' ((Read-SharedLog $sharingLog) -ceq '') 'an exclusive writer must leave this poll empty'
    $writer.Dispose(); $writer = $null
    Check 'reader-released' ((Read-SharedLog $sharingLog) -ceq $complete) 'complete record must survive writer release'
    $writer = [IO.File]::Open($sharingLog, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $partial = [Text.Encoding]::UTF8.GetBytes('reader partial')
    $writer.Write($partial, 0, $partial.Length); $writer.Flush()
    Check 'reader-partial' ((Read-SharedLog $sharingLog) -ceq $complete) 'incomplete append must not become a record'
    $newline = [Text.Encoding]::UTF8.GetBytes("`r`n")
    $writer.Write($newline, 0, $newline.Length); $writer.Flush()
    Check 'reader-completed' ((Read-SharedLog $sharingLog) -ceq ($complete + "reader partial`r`n")) 'completed append must become visible'
} finally {
    if ($writer) { $writer.Dispose() }
    if (Test-Path -LiteralPath $sharingLog) { Remove-Item -LiteralPath $sharingLog -Force }
}

# A process name no installer can produce, so the kill list is always empty even without -DryRun.
$victim = 'bc250-hostwatch-selftest-absent'
$log = Join-Path $Out 'killswitch.log'
$stop = Join-Path $Out 'killswitch.stop'
New-Item -ItemType File -Path $stop -Force | Out-Null   # stale: the run must clear it and keep watching
$argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'killswitch.ps1'),
          '-GrowthLimitMB', '-1', '-ProcessName', $victim, '-Log', $log, '-StopFile', $stop, '-DryRun')
$proc = Start-Process -FilePath $ps51 -ArgumentList $argv -WindowStyle Hidden -PassThru
try {
    # The first Win32_PerfFormattedData_PerfOS_Memory query of a session can take several seconds, so wait for the
    # evidence instead of a fixed sleep.
    $text = ''
    for ($i = 0; $i -lt 60 -and -not ($text -match 'dry run, nothing killed'); $i++) {
        Start-Sleep -Milliseconds 500
        $text = Read-SharedLog $log
    }
    $running = -not $proc.HasExited
    $fired = ($text -match 'KILL .*over limit') -and ($text -match 'dry run, nothing killed')
    Check 'trigger' ($running -and $fired) "still running=$running, log=$($text -replace '\s+', ' ')"

    New-Item -ItemType File -Path $stop -Force | Out-Null
    $exited = $proc.WaitForExit(20000)
    $text = Read-SharedLog $log
    Check 'stop' ($exited -and $text -match 'stop \(stop file seen\)') "exited=$exited"
} finally { Stop-SelftestChild $proc }

$hwLog = Join-Path $Out 'hostwatch.log'
$hw = Start-Process -FilePath $ps51 -WindowStyle Hidden -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'hostwatch.ps1'),
    '-Log', $hwLog, '-Every', '1')
try {
    $lines = @()
    for ($i = 0; $i -lt 60 -and $lines.Count -lt 2; $i++) {
        Start-Sleep -Milliseconds 500
        $lines = @((Read-SharedLog $hwLog) -split "`r?`n" | Where-Object { $_ -match 'npp=\d' })
    }
} finally { Stop-SelftestChild $hw }
Check 'sample' ($lines.Count -ge 2) "$($lines.Count) sample line(s)"

if ($failed.Count) { "FAILED: $($failed -join ', ')"; exit 1 }
'PASS hostwatch selftest (4 reader controls + 4 live checks)'
exit 0
