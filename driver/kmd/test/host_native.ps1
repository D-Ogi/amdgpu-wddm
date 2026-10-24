# Bounded, windowless run of a native host test program (BD-235). Dot-source this file, then call Invoke-HostTest.
# - The child starts without a window, with redirected output, and inherits an error mode without the Windows Error
#   Reporting and critical-error dialogs (SetErrorMode is inherited by child processes).
# - It runs for at most -TimeoutSeconds. On the deadline the process tree is stopped and the call throws.
# - The output is kept in -Log. The run counts only with exactly one summary line that matches -Summary, whose
#   groups 'checks' and 'failures' are numbers, and the FAIL lines must equal the failures in it. Otherwise it throws.
# A crash, a deadline or a missing summary is therefore never a test result: a negative control that ends that way is
# an invalid control, not a pass.
if (-not ('Bc250HostErrorMode' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class Bc250HostErrorMode {
    [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
}
'@
}

function Invoke-HostTest([string]$Exe, [string]$Log, [string]$Summary, [int]$TimeoutSeconds = 60) {
    # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, for this process and its children.
    $before = [Bc250HostErrorMode]::SetErrorMode(0x8003)
    try {
        $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        $p = [Diagnostics.Process]::Start($psi)
    } finally {
        [void][Bc250HostErrorMode]::SetErrorMode($before)
    }
    try {
        $stderrTask = $p.StandardError.ReadToEndAsync(); $stdoutTask = $p.StandardOutput.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { throw "$(Split-Path -Leaf $Exe) did not end in $TimeoutSeconds s" }
        $p.WaitForExit(); $code = $p.ExitCode
    } finally {
        if (-not $p.HasExited) { $p.Kill($true); $p.WaitForExit() }
    }
    $lines = @(($stdoutTask.Result + $stderrTask.Result) -split "`r?`n" | Where-Object { $_ -ne '' })
    $lines | Set-Content -LiteralPath $Log -Encoding utf8
    $lines | Write-Host
    Write-Host "$(Split-Path -Leaf $Exe) exit code $code"
    $found = @($lines | ForEach-Object { if ($_ -match $Summary) { $Matches } })
    if ($found.Count -ne 1) { throw "$(Split-Path -Leaf $Exe): $($found.Count) summary lines, the run did not complete" }
    $checks = [int]$found[0]['checks']; $failures = [int]$found[0]['failures']
    $failLines = @($lines -match '^FAIL').Count
    if ($failures -ne $failLines) { throw "$(Split-Path -Leaf $Exe): summary says $failures failures, $failLines FAIL lines" }
    if (($failures -eq 0) -ne ($code -eq 0)) { throw "$(Split-Path -Leaf $Exe): exit code $code with $failures failures" }
    return [pscustomobject]@{ Code = $code; Checks = $checks; Failures = $failures }
}
