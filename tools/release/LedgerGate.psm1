#Requires -Version 7.0
# The verdict of the work ledger gate of a release set build (build-release-set.ps1, set and -Plan mode), BD-334.
#
# The set build runs 'ledger.py check --release <version>' before the first build. A release candidate must be
# buildable while later work for the same tester release is still open (rc3 while rc4 entries serve tester.38), and
# ledger.py refuses a deferral to the same tester release by design. -LedgerDebtReason records that debt, as -Package
# does for its own ledger gate:
#   exit 0                                   PASS
#   exit 1 with its summary, no reason       FAIL (the set is not built)
#   exit 1 with its summary, a reason        DEBT: every ERROR line stays in the gate, followed by 'debt: <reason>'
#   any other end                            FAIL whatever the reason: the checker did not finish its check (a Python
#                                            traceback also ends with exit 1, but without the 'N error(s)' summary)
# DEBT is not FAIL, so the set builds; the report keeps the reason. -Package of that set runs its own ledger gate
# again and needs its own -LedgerDebtReason: a set debt does not carry over.

function Get-LedgerGate {
    param([Parameter(Mandatory)][int]$ExitCode, [string[]]$Output = @(), [Parameter(Mandatory)][string]$Version,
          [string]$DebtReason = '')
    $text = @($Output | ForEach-Object { ([string]$_).Trim() })
    $lines = @($text | Where-Object { $_ -cmatch '^ERROR\s|^\d+ error\(s\)' })
    if (-not $lines.Count) { $lines = @("ledger.py check --release ${Version}: exit $ExitCode") }
    $reported = $ExitCode -eq 1 -and @($text | Where-Object { $_ -cmatch '^\d+ error\(s\)' }).Count -gt 0
    $reason = "$DebtReason".Trim()
    $status = 'FAIL'
    if ($ExitCode -eq 0) { $status = 'PASS' }
    elseif ($reported -and $reason) { $status = 'DEBT'; $lines += "debt: $reason" }
    elseif (-not $reported) {
        $tail = @($text | Where-Object { $_ } | Select-Object -Last 3)
        $lines += @("the work ledger checker did not finish its check (exit $ExitCode, no error summary); a debt reason does not cover that") + $tail
    }
    [ordered]@{ name = 'ledger'; status = $status; lines = @($lines) }
}

Export-ModuleMember -Function Get-LedgerGate
