# Gate host-native: the rules of host_native.ps1 (Invoke-HostTest), checked with child fixtures (BD-235). Each fixture
# is a short pwsh child that prints test-like lines and exits with a chosen code. No native test, no crash, no lab.
# A valid run is accepted: no failures with exit 0, or CHECK failures with exit 1. Every other end must be refused,
# and for the reason expected: another exit code after a valid negative summary (2, and 0xC0000005 as a crash would
# give), a failure summary with exit 0, exit 1 with no failures, no summary, two summaries, a FAIL count that is not
# the summary's, and the deadline.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\host-native",
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\host_native.ps1"
$pwsh = (Get-Process -Id $PID).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'fixture.log'
$summary = '^fixture: (?<checks>\d+) checks, (?<failures>\d+) failures$'
$cases = 0; $bad = 0

function Fixture([string]$Script) { @('-NoProfile', '-NonInteractive', '-Command', $Script) }

function Accept([string]$Name, [string]$Script, [int]$Failures, [int]$Code) {
    $script:cases++
    try {
        $r = Invoke-HostTest $pwsh $log $summary $TimeoutSeconds (Fixture $Script) 6> $null
        if ($r.Failures -ne $Failures -or $r.Code -ne $Code) {
            Write-Output "FAIL $Name : accepted with $($r.Failures) failures, exit $($r.Code)"; $script:bad++
        } else { Write-Output "accepted as required: $Name" }
    } catch { Write-Output "FAIL $Name : refused: $($_.Exception.Message)"; $script:bad++ }
}

function Refuse([string]$Name, [string]$Script, [string]$Reason, [int]$Timeout = $TimeoutSeconds) {
    $script:cases++
    try {
        $r = Invoke-HostTest $pwsh $log $summary $Timeout (Fixture $Script) 6> $null
        Write-Output "FAIL $Name : accepted (failures $($r.Failures), exit $($r.Code))"; $script:bad++
    } catch {
        if ($_.Exception.Message -match $Reason) { Write-Output "refused as required: $Name ($($_.Exception.Message))" }
        else { Write-Output "FAIL $Name : refused for another reason: $($_.Exception.Message)"; $script:bad++ }
    }
}

$two = "'FAIL line 1: a'; 'FAIL line 2: b'; 'fixture: 9 checks, 2 failures'"
Accept 'no failures, exit 0' "'fixture: 9 checks, 0 failures'; exit 0" 0 0
Accept 'two CHECK failures, exit 1' "$two; exit 1" 2 1
Refuse 'a valid negative summary, exit 2' "$two; exit 2" 'exit code 2 with 2 failures, 1 expected'
Refuse 'a valid negative summary, exit 0xC0000005' "$two; exit -1073741819" 'exit code -1073741819 with 2 failures, 1 expected'
Refuse 'a failure summary, exit 0' "$two; exit 0" 'exit code 0 with 2 failures, 1 expected'
Refuse 'no failures, exit 1' "'fixture: 9 checks, 0 failures'; exit 1" 'exit code 1 with 0 failures, 0 expected'
Refuse 'no summary' "'FAIL line 1: a'; exit 1" '0 summary lines'
Refuse 'two summaries' "'fixture: 9 checks, 0 failures'; 'fixture: 9 checks, 0 failures'; exit 0" '2 summary lines'
Refuse 'FAIL lines not the summary count' "'FAIL line 1: a'; 'fixture: 9 checks, 2 failures'; exit 1" 'summary says 2 failures, 1 FAIL lines'
Refuse 'the deadline' "Start-Sleep -Seconds 60; 'fixture: 9 checks, 0 failures'; exit 0" 'did not end in 2 s' 2
Write-Output "host_native: $cases cases, $bad failures"
exit $(if ($bad) { 1 } else { 0 })
