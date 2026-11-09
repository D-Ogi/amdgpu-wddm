#Requires -Version 7.0
# Host test of LedgerGate.psm1 (BD-334): the work ledger gate of a release set build is PASS on a clean ledger, FAIL
# on open work of the release without -LedgerDebtReason, DEBT with it (every ERROR line kept, then the reason), and
# FAIL with or without a reason when the checker does not finish. The real checker runs on a small ledger under -Out.
#   pwsh -File tools\release\test-ledger-gate.ps1 -Out <dir> [-Ledger <scratch\ledger\ledger.py>]
#   (the 'ledger-gate' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out, [string]$Ledger = '')
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'LedgerGate.psm1') -Force
if (-not $Ledger) {
    if (-not $env:BC250_ROOT) { throw 'give -Ledger <scratch\ledger\ledger.py>, or set BC250_ROOT' }
    $Ledger = Join-Path $env:BC250_ROOT 'scratch\ledger\ledger.py'
}
if (-not (Test-Path -LiteralPath $Ledger)) { throw "no work ledger checker at $Ledger" }
$python = (Get-Command python -ErrorAction Stop).Source
$version = '0.7.216.999-tester.99'
$reason = 'rc3 is a candidate of tester.99; the rc4 work of the same release is open'
$passed = 0
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }

# 1. The verdict, from the lines that ledger.py writes.
$errorOut = @('release gate for 0.7.216.999-tester.99: 2 entries considered', 'ledger check: 2 entries',
    'ERROR  release         0.7.216.999-tester.99: a-open serves tester.99 in state in-progress with no landed_in',
    'ERROR  not-landed      b-open: landed_in main:1234567 is not on origin/main', 'warn   criterion-debt  none',
    '', '2 error(s), 1 warning(s)', '  errors by rule: not-landed 1, release 1')
$g = Get-LedgerGate -ExitCode 0 -Output @('ledger check: 0 entries', '', '0 error(s), 0 warning(s)') -Version $version
Assert ($g.status -eq 'PASS') "exit 0 is PASS, not $($g.status)"
$g = Get-LedgerGate -ExitCode 1 -Output $errorOut -Version $version
Assert ($g.status -eq 'FAIL') "errors without a reason are FAIL, not $($g.status)"
Assert (-not @($g.lines | Where-Object { $_ -like 'debt:*' }).Count) 'a FAIL gate has no debt line'
foreach ($blank in '', '   ') {
    $g = Get-LedgerGate -ExitCode 1 -Output $errorOut -Version $version -DebtReason $blank
    Assert ($g.status -eq 'FAIL') "a blank reason is no reason: '$blank' gives $($g.status)"
}
$g = Get-LedgerGate -ExitCode 1 -Output $errorOut -Version $version -DebtReason "  $reason "
Assert ($g.status -eq 'DEBT') "errors with a reason are DEBT, not $($g.status)"
Assert (@($g.lines | Where-Object { $_ -like 'ERROR *' }).Count -eq 2) "DEBT keeps both ERROR lines: $($g.lines -join ' | ')"
Assert ($g.lines[-1] -eq "debt: $reason") "the last DEBT line is the trimmed reason, not '$($g.lines[-1])'"
Assert (-not @($g.lines | Where-Object { $_ -like 'warn*' }).Count) 'warnings stay out of the gate lines'
$traceback = @('Traceback (most recent call last):', '  File "ledger.py", line 173, in load',
    'json.decoder.JSONDecodeError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)')
$g = Get-LedgerGate -ExitCode 1 -Output $traceback -Version $version -DebtReason $reason
Assert ($g.status -eq 'FAIL') "a traceback (exit 1, no summary) is FAIL with a reason too, not $($g.status)"
Assert (@($g.lines | Where-Object { $_ -like '*JSONDecodeError*' }).Count -eq 1) 'the FAIL names the last lines of the traceback'
$g = Get-LedgerGate -ExitCode 2 -Output @('ledger: LEDGER.json is missing') -Version $version -DebtReason $reason
Assert ($g.status -eq 'FAIL') "exit 2 is FAIL with a reason too, not $($g.status)"

# 2. The real checker on a small workspace: the release sources name one component, the size record is fresh.
$root = Join-Path ([IO.Path]::GetFullPath($Out)) ('ledger-gate-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force (Join-Path $root 'scratch\cleanup-250') | Out-Null
[IO.File]::WriteAllText((Join-Path $root 'release-sources.json'), '{"files": [{"component": "kmd", "path": "amdgpu_wddm.sys"}]}')
[IO.File]::WriteAllText((Join-Path $root 'notes.md'), 'the open work')
$now = [DateTimeOffset]::UtcNow
[IO.File]::WriteAllText((Join-Path $root 'scratch\cleanup-250\size-latest.json'), (ConvertTo-Json @{ epoch = $now.ToUnixTimeSeconds()
            utc = $now.ToString('yyyy-MM-ddTHH:mm:ssZ'); counted_bytes = 1000; free_bytes = 500000000000 }))
$ignore = '[{"pattern": "scratch/cleanup-250", "reason": "the size record of this test"}]'
function Set-Ledger([string]$Entries) {
    [IO.File]::WriteAllText((Join-Path $root 'LEDGER.json'), "{`"release_sources`": `"release-sources.json`", `"repos`": {}, `"ignore`": $ignore, `"census`": [`"scratch/cleanup-250`"], `"entries`": $Entries}")
}
function Get-Gate([string]$DebtReason) {
    $o = @(& $python -I $Ledger --root $root check --release $version 2>&1 | ForEach-Object { [string]$_ })
    Get-LedgerGate -ExitCode $LASTEXITCODE -Output $o -Version $version -DebtReason $DebtReason
}
Set-Ledger '[]'
$g = Get-Gate ''
Assert ($g.status -eq 'PASS') "a clean ledger is PASS, not $($g.status): $($g.lines -join ' | ')"
$g = Get-Gate $reason
Assert ($g.status -eq 'PASS' -and -not @($g.lines | Where-Object { $_ -like 'debt:*' }).Count) 'a reason on a clean ledger records no debt'
Set-Ledger "[{`"id`": `"t99-open`", `"kind`": `"doc`", `"where`": `"notes.md`", `"state`": `"in-progress`", `"what`": `"open work of the release`", `"serves`": [`"tester.99`"], `"updated`": `"$($now.ToString('yyyy-MM-dd'))`", `"components`": [`"kmd`"]}]"
$g = Get-Gate ''
Assert ($g.status -eq 'FAIL') "open work of the release is FAIL without a reason, not $($g.status)"
Assert (@($g.lines | Where-Object { $_ -like 'ERROR*t99-open*' }).Count -ge 1) "the FAIL names the open entry: $($g.lines -join ' | ')"
$g = Get-Gate $reason
Assert ($g.status -eq 'DEBT') "open work of the release is DEBT with a reason, not $($g.status)"
Assert (@($g.lines | Where-Object { $_ -like 'ERROR*t99-open*' }).Count -ge 1 -and $g.lines[-1] -eq "debt: $reason") "the DEBT keeps the entry and the reason: $($g.lines -join ' | ')"
[IO.File]::WriteAllText((Join-Path $root 'LEDGER.json'), '{broken')
$g = Get-Gate $reason
Assert ($g.status -eq 'FAIL') "a malformed LEDGER.json is FAIL with a reason too, not $($g.status)"

# 3. build-release-set.ps1 takes the reason in the set build and in -Package, and -Package keeps its own gate.
$cmd = Get-Command (Join-Path $PSScriptRoot 'build-release-set.ps1')
$sets = @($cmd.ParameterSets | Where-Object { $_.Parameters.Name -contains 'LedgerDebtReason' } | ForEach-Object { $_.Name })
Assert ($sets -contains 'Set' -and $sets -contains 'Package') "-LedgerDebtReason binds in Set and Package, not only in $($sets -join ', ')"
$text = Get-Content -Raw -LiteralPath $cmd.Source
Assert ($text -match 'Get-LedgerGate -ExitCode \$ledgerExit[^\r\n]*-DebtReason \$LedgerDebtReason') 'the set gate gives the reason to Get-LedgerGate'
Assert ($text -match "Stop-Package 'the work ledger refuses this release") '-Package still runs its own ledger gate'
Assert ($text -match 'ledger_debt_reason = ') 'the report records the reason'

Remove-Item -LiteralPath $root -Recurse -Force
Write-Host "PASS: $passed checks (the ledger gate: PASS, FAIL, DEBT, and FAIL when the checker does not finish)"
