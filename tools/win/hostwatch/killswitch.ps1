# Kill switch for a watched live kd.exe session on the development PC (owner's consent 2026-09-27).
# Every second: read the nonpaged pool; if it exceeds -LimitMB, or grows by more than -RateMBps in one second,
# kill every kd.exe, log why, and keep watching (the 2026-09-21 leak released memory ~2 min after the kill).
# Run it BEFORE `kd_server.py start`; stop it with Ctrl+C or by deleting the -StopFile.
# Validated 2026-09-27 19:10 with killswitch-dryrun.ps1 against a hidden dummy ping (killswitch-dryrun.log): the
# -DryRun pass logged the trigger and left the process alive, the real pass killed it within one second.
# Nonpaged baseline at that time: 4824 MB, so the default GrowthLimitMB 3072 puts the cap near 7.9 GB.
param(
    [int]$GrowthLimitMB = 3072,      # hard cap = nonpaged pool at start + this (the host idled near 5 GB on 2026-09-27, so no fixed ceiling)
    [int]$RateMBps = 300,            # one-second growth that means "the leak is on" (observed: ~870 MB/s)
    # BC250_ROOT is the workspace root; by default the parent directory of this repository. The log and the stop
    # file stay under its scratch directory, never on drive C:.
    [string]$StateDir = (Join-Path (& { if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path } }) 'scratch\hostwatch'),
    [string]$Log,
    [string]$StopFile,
    [string]$ProcessName = 'kd',
    [switch]$DryRun
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $StateDir | Out-Null
if (-not $Log) { $Log = Join-Path $StateDir 'killswitch.log' }
if (-not $StopFile) { $StopFile = Join-Path $StateDir 'killswitch.stop' }
if (Test-Path -LiteralPath $StopFile) { Remove-Item -LiteralPath $StopFile -Force }
$prev = $null
$baseline = [math]::Round((Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory).PoolNonpagedBytes / 1MB)
$LimitMB = $baseline + $GrowthLimitMB
Add-Content -Path $Log -Value ('{0} start baseline={1}MB limit={2}MB rate={3}MB/s process={4} dryrun={5}' -f (Get-Date).ToString('s'), $baseline, $LimitMB, $RateMBps, $ProcessName, [bool]$DryRun)
while (-not (Test-Path -LiteralPath $StopFile)) {
    try {
        $m = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
        $npp = [math]::Round($m.PoolNonpagedBytes / 1MB)
        $delta = if ($null -ne $prev) { $npp - $prev } else { 0 }
        $prev = $npp
        $reason = $null
        if ($npp -gt $LimitMB) { $reason = "nonpaged $npp MB over limit $LimitMB MB" }
        elseif ($delta -gt $RateMBps) { $reason = "nonpaged grew $delta MB in one second" }
        if ($reason) {
            $victims = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
            $line = '{0} KILL {1}: {2} process(es) [{3}]' -f (Get-Date).ToString('s'), $reason, $victims.Count, (($victims | ForEach-Object { $_.Id }) -join ',')
            if (-not $DryRun) { $victims | ForEach-Object { try { Stop-Process -Id $_.Id -Force -ErrorAction Stop } catch { $line += " (kill $($_.Id) failed: $($_.Exception.Message))" } } }
            else { $line += ' (dry run, nothing killed)' }
            Add-Content -Path $Log -Value $line
            Write-Host $line
        }
    } catch {
        Add-Content -Path $Log -Value ('{0} error {1}' -f (Get-Date).ToString('s'), $_.Exception.Message)
    }
    Start-Sleep -Seconds 1
}
Add-Content -Path $Log -Value ('{0} stop (stop file seen)' -f (Get-Date).ToString('s'))
