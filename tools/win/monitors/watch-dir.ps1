# Prints one line for each poll that finds a new or a changed file in a directory.
# Written for a watcher that reads this script's standard output and reports each line as it arrives.
# The directory can be outside this repository, so give its path on the command line.
# Usage: watch-dir.ps1 -Path <dir> [-Filter *.md] [-Minutes 0] [-IntervalSeconds 10] [-ExitOnChange] [-Label text]
#   -Minutes 0      run until the operator stops the process
#   -Minutes N      stop after N minutes. Print "no change in N min" if nothing changed
#   -ExitOnChange   exit with code 0 after the first line
# Exit codes: 0 done, 2 the directory does not exist.
param(
    [Parameter(Mandatory)][string]$Path,
    [string]$Filter = '*',
    [int]$Minutes = 0,
    [int]$IntervalSeconds = 10,
    [switch]$ExitOnChange,
    [string]$Label = 'changed'
)
if (!(Test-Path -LiteralPath $Path -PathType Container)) {
    [Console]::Error.WriteLine("watch-dir: no directory at $Path")
    exit 2
}
# One record per file: name, write time and length. A changed record means a new or an edited file.
function Get-State {
    @(Get-ChildItem -LiteralPath $Path -Filter $Filter -File -ErrorAction Stop |
        ForEach-Object { '{0}|{1}|{2}' -f $_.Name, $_.LastWriteTimeUtc.Ticks, $_.Length })
}
$prev = Get-State
$reported = 0
$deadline = if ($Minutes -gt 0) { (Get-Date).AddMinutes($Minutes) } else { [DateTime]::MaxValue }
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $IntervalSeconds
    try { $cur = Get-State } catch { continue }
    $changed = @($cur | Where-Object { $prev -notcontains $_ } | ForEach-Object { ($_ -split '\|')[0] })
    $prev = $cur
    if ($changed.Count -gt 0) {
        '{0}: {1}' -f $Label, (($changed | Sort-Object -Unique) -join ', ')
        [Console]::Out.Flush()
        $reported++
        if ($ExitOnChange) { exit 0 }
    }
}
if ($reported -eq 0 -and $Minutes -gt 0) { 'no change in {0} min' -f $Minutes }
