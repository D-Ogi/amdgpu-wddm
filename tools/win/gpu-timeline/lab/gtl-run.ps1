# Runs on unit A (Windows PowerShell 5.1, elevated): one bounded gpu-timeline run with its preflight and receipt.
#
#   gtl-run.ps1 -Tag 310-low [-Seconds 60] [-Hz 1009] [-Set full|lite] [-ProbeOnly] [-Detach]
#
# Preflight: gpu-timeline.exe present (hash recorded), bc250rd driver running (its .sys path and hash recorded),
# output directory C:\BC250\tmp\gtl\<Tag> new. Then a probe (one read of each register printed, 200 batched reads
# timed: under a second) and, unless -ProbeOnly, one sample run of at most 60 s. Everything lands in the output
# directory: probe.txt, sample.txt, run.gtl, meta.json. Early stop: create <outdir>\stop (the tool checks every
# ~100 ms and still writes what it has).
# -Detach: the same run as a one-shot SYSTEM scheduled task (no SSH session held for the minute); the call returns
# at once, the task removes itself at the end. Pull the directory afterwards.
# Reads only: the tool sends bc250rd's ATTACH and READ IOCTLs, never its SMU or SMN ones.
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]{1,40}$')][string]$Tag,
    [ValidateRange(1, 60)][int]$Seconds = 60,
    [ValidateRange(100, 5000)][int]$Hz = 1009,
    [ValidateSet('full', 'lite')][string]$Set = 'full',
    [string]$Exe = 'C:\BC250\gpu-timeline\gpu-timeline.exe',
    [string]$ExpectExeSha256 = '',
    [switch]$ProbeOnly,
    [switch]$Detach,
    [string]$TaskName = ''
)
$ErrorActionPreference = 'Stop'
$out = "C:\BC250\tmp\gtl\$Tag"

if ($Detach) {
    if (Test-Path -LiteralPath $out) { throw "output directory $out exists: pick a new tag" }
    $name = "GpuTimeline-$Tag"
    if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) { throw "task $name already registered" }
    $null = New-Item -ItemType Directory -Force -Path 'C:\BC250\tmp\gtl'
    $copy = "C:\BC250\tmp\gtl\gtl-run-$Tag.ps1"
    Copy-Item -LiteralPath $MyInvocation.MyCommand.Path -Destination $copy
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$copy`" -Tag $Tag -Seconds $Seconds -Hz $Hz -Set $Set " +
               "-Exe `"$Exe`" -TaskName $name"
    if ($ExpectExeSha256) { $argList += " -ExpectExeSha256 $ExpectExeSha256" }
    if ($ProbeOnly) { $argList += ' -ProbeOnly' }
    $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argList
    $s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds + 60)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $null = Register-ScheduledTask -TaskName $name -Action $a -Settings $s -Principal $p
    Start-ScheduledTask -TaskName $name
    "started task $name utc $([DateTime]::UtcNow.ToString('o')) seconds $Seconds out $out"
    return
}

# The tool as a child process with both streams to <Base>.txt / <Base>-err.txt, as written (PS 5.1's *> re-encodes as
# UTF-16, and 2>&1 under ErrorActionPreference Stop turns a stderr line into a terminating error). Killed after
# $LimitSeconds (the tool bounds itself; this bounds the wrapper). Returns the exit code, -1 when it was killed.
function Invoke-Gtl([string]$Arguments, [string]$Base, [int]$LimitSeconds) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $stderr = $proc.StandardError.ReadToEndAsync()
    $code = -1
    if ($proc.WaitForExit($LimitSeconds * 1000)) { $proc.WaitForExit(); $code = $proc.ExitCode } else { $proc.Kill() }
    [System.IO.File]::WriteAllText("$Base.txt", $stdout.Result)
    [System.IO.File]::WriteAllText("$Base-err.txt", $stderr.Result)
    return $code
}

$meta = [ordered]@{ tag = $Tag; seconds = $Seconds; hz = $Hz; set = $Set; probe_only = [bool]$ProbeOnly;
                    task = $TaskName; utc_start = [DateTime]::UtcNow.ToString('o') }
$created = $false
try {
    if (Test-Path -LiteralPath $out) { throw "output directory $out exists: pick a new tag" }
    if (!(Test-Path -LiteralPath $Exe)) { throw "$Exe not found" }
    $meta.exe = $Exe
    $meta.exe_sha256 = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    if ($ExpectExeSha256 -and $meta.exe_sha256 -ne $ExpectExeSha256.ToUpperInvariant()) {
        throw "gpu-timeline.exe hash $($meta.exe_sha256) is not the expected $ExpectExeSha256"
    }
    $drv = Get-CimInstance Win32_SystemDriver -Filter "Name='bc250rd'"
    if ($null -eq $drv) { throw 'bc250rd driver service not installed' }
    $meta.bc250rd_state = $drv.State
    $meta.bc250rd_path = $drv.PathName
    $sys = $drv.PathName -replace '^\\\?\?\\', ''
    if ($sys -match '^\\SystemRoot\\') { $sys = $sys -replace '^\\SystemRoot', $env:SystemRoot }
    if (Test-Path -LiteralPath $sys) { $meta.bc250rd_sha256 = (Get-FileHash -LiteralPath $sys -Algorithm SHA256).Hash }
    if ($drv.State -ne 'Running') { throw "bc250rd is $($drv.State), not Running (start it with sc.exe start bc250rd)" }
    $null = New-Item -ItemType Directory -Force -Path $out
    $created = $true

    $probeExit = Invoke-Gtl "probe --set $Set --reads 200" (Join-Path $out 'probe') 30
    $meta.probe_exit = $probeExit
    if ($probeExit -ne 0) { throw "probe failed ($probeExit), see probe-err.txt" }
    if (!$ProbeOnly) {
        $meta.utc_sample_start = [DateTime]::UtcNow.ToString('o')
        $meta.sample_exit = Invoke-Gtl ("sample --seconds $Seconds --hz $Hz --set $Set --out `"$(Join-Path $out 'run.gtl')`" " +
            "--stop-file `"$(Join-Path $out 'stop')`"") (Join-Path $out 'sample') ($Seconds + 30)
        $meta.utc_sample_end = [DateTime]::UtcNow.ToString('o')
        if (Test-Path -LiteralPath (Join-Path $out 'run.gtl')) {
            $meta.run_sha256 = (Get-FileHash -LiteralPath (Join-Path $out 'run.gtl') -Algorithm SHA256).Hash
        }
    }
    $meta.result = 'done'
} catch {
    $meta.result = 'refused'
    $meta.error = $_.Exception.Message
} finally {
    $meta.utc_end = [DateTime]::UtcNow.ToString('o')
    # The receipt also lands next to the directory, so a refused (or detached) run leaves one even without $out.
    $null = New-Item -ItemType Directory -Force -Path 'C:\BC250\tmp\gtl'
    $meta | ConvertTo-Json | Set-Content -LiteralPath "C:\BC250\tmp\gtl\$Tag-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')).meta.json" -Encoding ascii
    if ($created) {
        $meta | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'meta.json') -Encoding ascii
        foreach ($f in 'probe.txt', 'probe-err.txt', 'sample.txt', 'sample-err.txt') {
            $p = Join-Path $out $f
            if (Test-Path -LiteralPath $p) { "---- $f"; Get-Content -LiteralPath $p }
        }
    }
    $meta | ConvertTo-Json
    if ($TaskName) { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue }
}
