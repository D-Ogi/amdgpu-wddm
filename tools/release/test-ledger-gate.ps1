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

# 4. BD-326 (b): the plan's pins file (build_release_set.py ledger-pins) holds a fork: landing at a commit that the
# manifest pins, the row commit and the gitlink of a submodule in it. Two fork clones: vk has dx as the submodule
# subprojects/dxil-spirv at commit D; the manifest row builds vk at A. Controls: without the pins file both landings
# are refused, and a pins file without the "repositories" table refuses the submodule landing (the review finding).
$root = Join-Path ([IO.Path]::GetFullPath($Out)) ('ledger-pins-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
function Invoke-Git([string]$Dir) {
    & git -C $Dir -c user.name=fixture -c user.email=research@example.com @args
    if ($LASTEXITCODE) { throw "git $args failed in $Dir" }
}
$vk = Join-Path $root 'fork\vk'; $dx = Join-Path $root 'fork\dx'
foreach ($d in $vk, $dx) {
    New-Item -ItemType Directory -Force $d | Out-Null
    Invoke-Git $d init -q; Invoke-Git $d symbolic-ref HEAD refs/heads/main
}
foreach ($t in 'one', 'two') { [IO.File]::WriteAllText("$dx\a.txt", $t); Invoke-Git $dx add a.txt; Invoke-Git $dx commit -q -m $t }
$dxPin = (& git -C $dx rev-parse HEAD).Trim()
[IO.File]::WriteAllText("$vk\v.txt", 'v'); Invoke-Git $vk add v.txt
Invoke-Git $vk update-index --add --cacheinfo "160000,$dxPin,subprojects/dxil-spirv"
Invoke-Git $vk commit -q -m 'the submodule pin'
$vkPin = (& git -C $vk rev-parse HEAD).Trim()
New-Item -ItemType Directory -Force (Join-Path $root 'scratch\cleanup-250') | Out-Null
[IO.File]::WriteAllText((Join-Path $root 'release-sources.json'), '{"files": [{"component": "kmd", "path": "amdgpu_wddm.sys"}]}')
[IO.File]::WriteAllText((Join-Path $root 'scratch\cleanup-250\size-latest.json'), (ConvertTo-Json @{ epoch = $now.ToUnixTimeSeconds()
            utc = $now.ToString('yyyy-MM-ddTHH:mm:ssZ'); counted_bytes = 1000; free_bytes = 500000000000 }))
$day = $now.ToString('yyyy-MM-dd')
[IO.File]::WriteAllText((Join-Path $root 'LEDGER.json'), (ConvertTo-Json -Depth 6 @{ release_sources = 'release-sources.json'
            repos = @{ vk = @{ path = 'fork/vk'; release_repo = 'vkd3d-proton' }; dx = @{ path = 'fork/dx'; release_repo = 'dxil-spirv' } }
            ignore = @(@{ pattern = 'scratch/cleanup-250'; reason = 'the size record of this test' }, @{ pattern = 'fork'; reason = 'the fork clones of this test' })
            census = @('scratch/cleanup-250', 'fork')
            entries = @(
                @{ id = 'vk-fix'; kind = 'fork-branch'; where = "vk:main@$vkPin"; what = 'a fork fix'; state = 'landed'; landed_in = "fork:vk:main@$vkPin"; updated = $day; serves = @('tester.99') },
                @{ id = 'dx-fix'; kind = 'fork-branch'; where = "dx:main@$dxPin"; what = 'a submodule fix'; state = 'landed'; landed_in = "fork:dx:main@$dxPin"; updated = $day; serves = @('tester.99') }) }))
$manifest = Join-Path $root 'manifest.json'
[IO.File]::WriteAllText($manifest, (ConvertTo-Json -Depth 6 @{ version = $version
            repositories = @{ 'vkd3d-proton' = @{ url = 'fixture'; checkouts = @('fork/vk'); submodules = @{ 'subprojects/dxil-spirv' = 'dxil-spirv' } }
                'dxil-spirv' = @{ url = 'fixture'; checkouts = @('fork/dx') } }
            rows = @(@{ id = 'd3d12-x64-rel'; repo = 'vkd3d-proton'; commit = $vkPin; flavour = 'release'; component = 'd3d12'; outputs = @(@{ path = 'x.dll' }) }) }))
$pins = Join-Path $root 'pins.json'
$o = @(& $python -I (Join-Path $PSScriptRoot 'build_release_set.py') ledger-pins --manifest $manifest --write $pins 2>&1 | ForEach-Object { [string]$_ })
Assert ($LASTEXITCODE -eq 0) "ledger-pins writes the pins file: $($o -join ' | ')"
function Get-PinGate([string[]]$Sources) {
    $extra = @(foreach ($s in $Sources) { '--release-sources'; $s })
    $o = @(& $python -I $Ledger --root $root check --release $version @extra 2>&1 | ForEach-Object { [string]$_ })
    Get-LedgerGate -ExitCode $LASTEXITCODE -Output $o -Version $version
}
$g = Get-PinGate @($pins)
Assert ($g.status -eq 'PASS') "the manifest pins hold both fork landings, the submodule one too, not $($g.status): $($g.lines -join ' | ')"
$g = Get-PinGate @()
Assert ($g.status -eq 'FAIL' -and @($g.lines | Where-Object { $_ -like 'ERROR*not-landed*vk-fix*' }).Count -eq 1 -and
    @($g.lines | Where-Object { $_ -like 'ERROR*not-landed*dx-fix*' }).Count -eq 1) "without the pins file both landings are refused: $($g.lines -join ' | ')"
$doc = Get-Content -Raw -LiteralPath $pins | ConvertFrom-Json -AsHashtable
$doc.Remove('repositories')
$filesOnly = Join-Path $root 'pins-files-only.json'
[IO.File]::WriteAllText($filesOnly, (ConvertTo-Json -Depth 8 $doc))
$g = Get-PinGate @($filesOnly)
Assert ($g.status -eq 'FAIL' -and @($g.lines | Where-Object { $_ -like 'ERROR*dx-fix*' }).Count -eq 1 -and
    -not @($g.lines | Where-Object { $_ -like 'ERROR*vk-fix*' }).Count) "without the repositories table only the submodule landing is refused: $($g.lines -join ' | ')"
Remove-Item -LiteralPath $root -Recurse -Force
Write-Host "PASS: $passed checks (the ledger gate: PASS, FAIL, DEBT, and FAIL when the checker does not finish; a fork landing at a manifest pin and at a submodule pin)"
