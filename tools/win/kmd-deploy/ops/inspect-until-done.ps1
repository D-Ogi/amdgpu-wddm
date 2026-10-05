# Operator helper: Inspect an attempt every 25 s until its task is no longer Running or 200 s pass.
param([Parameter(Mandatory)][string]$Attempt, [string]$Dispatch = 'dispatch.py')
$here = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$end = (Get-Date).AddSeconds(200)
while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds 25
    $out = & python -B (Join-Path $here $Dispatch) $Attempt Inspect 2>&1 | Out-String
    $state = if ($out -match '"state":\s*"([^"]+)"') { $Matches[1] } else { '?' }
    Write-Output ("{0:HH:mm:ss}Z state {1}" -f (Get-Date).ToUniversalTime(), $state)
    if ($state -ne 'Running') { Write-Output $out; break }
}
