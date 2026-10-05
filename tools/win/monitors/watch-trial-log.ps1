# Prints the new key lines of a trial log on the development PC, one line per event.
# Written for a watcher that reads this script's standard output and reports each line as it arrives.
# Usage: watch-trial-log.ps1 -Log <path> [-Minutes 7] [-IntervalSeconds 5] [-Width 220]
param(
    [Parameter(Mandatory)][string]$Log,
    [int]$Minutes = 7,
    [int]$IntervalSeconds = 5,
    [int]$Width = 220
)
# Lines worth reporting: step markers, failures, the STOP brake, traces, status and state, and the
# closure markers that end the watch.
$keep = '^> |step failed|STOP|^trace |status|state|archive|not closed|game'
$done = '^archive|not closed|closure or'
$n = 0
$end = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $end) {
    if (Test-Path -LiteralPath $Log) {
        $lines = @(Get-Content -LiteralPath $Log)
        if ($lines.Count -gt $n) {
            foreach ($l in $lines[$n..($lines.Count - 1)]) {
                if ($l -match $keep) { $l.Substring(0, [Math]::Min($Width, $l.Length)) }
            }
            $n = $lines.Count
            [Console]::Out.Flush()
            if ($lines -match $done) { break }
        }
    }
    Start-Sleep -Seconds $IntervalSeconds
}
