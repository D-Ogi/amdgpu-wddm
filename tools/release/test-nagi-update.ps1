# Host test of the apply side of Nagi self-update (docs/design/self-update.md, installer\nagi-update.ps1), in Windows
# PowerShell 5.1 as the installer runs it:
#   1. the apply plan over every case of nagi-update-plan-cases.json (the control application answers the same file);
#   2. the installed view (the manifest of the last full install, the update record, the apps layout);
#   3. the update manifest reader and the signature check of the committed fixture;
#   4. the atomic switch of the control folder on real junctions, with a stop after each step and its repair;
#   5. the apps pack reader (no file more, no file less, each SHA-256);
#   6. the transaction decision and the confirm step with stand-in actions;
#   7. the pinned signer list against the control application's list.
# Each of 1, 4 and 6 has negative controls that change the logic itself: the test runs its cases again against a
# copy of nagi-update.ps1 with one rule taken out, and that copy must fail them.
# Files go under -WorkRoot only: junctions and folders of a scratch install root. No registry, no administrator.
#
#   powershell -NoProfile -File tools\release\test-nagi-update.ps1 -WorkRoot <folder>
#
# Exit 0 = every check passed.
param([Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot 'installer'
$source = Join-Path $installer 'nagi-update.ps1'
. $source
$script:pass = 0; $script:fail = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { $script:pass++; Write-Host "PASS $What" } else { $script:fail++; Write-Host "FAIL $What" }
}
$WorkRoot = (New-Item -ItemType Directory -Force $WorkRoot).FullName
$run = Join-Path $WorkRoot ([Guid]::NewGuid().ToString('N').Substring(0, 8))
[void][IO.Directory]::CreateDirectory($run)

# A copy of nagi-update.ps1 with one exact text replaced. The replacement must hit, so that a later edit of the
# source cannot turn a negative control into a test of nothing.
function New-Mutant([string]$Name, [string]$Old, [string]$New) {
    $text = [IO.File]::ReadAllText($source)
    $at = $text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($at -lt 0) { throw "mutant ${Name}: the text to change is not in nagi-update.ps1" }
    $path = Join-Path $run "mutant-$Name.ps1"
    [IO.File]::WriteAllText($path, $text.Substring(0, $at) + $New + $text.Substring($at + $Old.Length), (New-Object Text.UTF8Encoding $false))
    return $path
}

# ---- 1. the apply plan -----------------------------------------------------------------------------------------------
$casesFile = Join-Path $PSScriptRoot 'nagi-update-plan-cases.json'
$planCases = (Get-Content -LiteralPath $casesFile -Raw | ConvertFrom-Json).cases
function Invoke-PlanCases($Cases) {
    $bad = @()
    foreach ($c in $Cases) {
        $p = Get-NagiApplyPlan -Manifest $c.manifest -Installed $c.installed
        $names = @($p.actions | ForEach-Object { [string]$_.name })
        $want = @($c.expect.actions)
        $ok = ($p.outcome -eq $c.expect.outcome) -and ([string]$p.reason -eq [string]$c.expect.reason) -and
            (($names -join ',') -eq ($want -join ',')) -and ([bool]$p.restart -eq [bool]$c.expect.restart)
        if (-not $ok) { $bad += "$($c.name): $($p.outcome) $($p.reason) [$($names -join ',')] restart $($p.restart)" }
    }
    return , $bad
}
$bad = Invoke-PlanCases $planCases
foreach ($c in $planCases) { Check (-not ($bad | Where-Object { $_.StartsWith("$($c.name):") })) "plan: $($c.name)" }
Check (@($planCases).Count -ge 18) "plan: $(@($planCases).Count) cases read"
$planMutants = @(
    @{ n = 'no-downgrade-rule'; o = "if (`$cmp -lt 0) { return (Plan 'refused' 'downgrade' @() `$false) }"; r = '' },
    @{ n = 'no-apps-compare'; o = "([string]`$app.content_id -ne [string]`$Installed.apps.content_id)"; r = '$false' },
    @{ n = 'no-abi-rule'; o = "@(`$abis | Where-Object { ([string]`$_).ToUpperInvariant() -eq `$finalAbi.ToUpperInvariant() }).Count -eq 0 -or "; r = '' },
    @{ n = 'no-min-installed'; o = "if ((Compare-NagiRelease ([string]`$Installed.release) ([string]`$Manifest.min_installed)) -lt 0) { return (Plan 'refused' 'too-old' @() `$false) }"; r = '' }
)
foreach ($mu in $planMutants) {
    $file = New-Mutant $mu.n $mu.o $mu.r
    $mbad = & { . $file; Invoke-PlanCases $planCases }
    Check (@($mbad).Count -gt 0) "plan negative control $($mu.n): the cases catch it ($(@($mbad).Count) case(s) fail: $(@($mbad)[0]))"
}

# ---- 2. the installed view -------------------------------------------------------------------------------------------
$m36 = [pscustomobject]@{ release = '0.7.216.108-tester.36'; kmd_abi = '0x000700D8'
    components = @([pscustomobject]@{ package_path = 'payload/kmd/bc250kmd.sys'; sha256 = ('1' * 64) }) }
$v = Get-NagiInstalledView -Manifest $m36 -Record $null -Layout ([pscustomobject]@{ kind = 'classic'; target_leaf = $null })
Check ($v.release -eq '0.7.216.108-tester.36' -and $null -eq $v.driver.content_id -and $v.driver.state_schema -eq 2 -and $v.driver.kmd_sha256 -eq ('1' * 64) -and
    @($v.apps.kmd_abis) -contains '0x000700D8') 'view: a release before the update identities has no driver content id, schema 2, its own ABI for its apps'
$m37 = [pscustomobject]@{ release = '0.7.216.109-tester.37'; kmd_abi = '0x000700D8'; components = @()
    update_components = [pscustomobject]@{ driver = [pscustomobject]@{ content_id = ('A' * 64); state_schema = 2; catalog_id = ('c' * 64) }; apps = [pscustomobject]@{ content_id = ('B' * 64) } } }
$rec = [pscustomobject]@{ schema = 'nagi.installed/1'; release = '0.7.216.110-tester.38'; driver_content_id = ('A' * 64)
    apps = [pscustomobject]@{ version = '0.7.216.110-tester.38'; folder = '0.7.216.110-tester.38'; content_id = ('D' * 64); kmd_abis = @('0x000700D8'); state_schemas = @(2) }
    data = [pscustomobject]@{ profiles = [pscustomobject]@{ version = '0.7.216.110-tester.38'; content_id = ('E' * 64) } } }
$j38 = [pscustomobject]@{ kind = 'junction'; target_leaf = '0.7.216.110-tester.38' }
$v = Get-NagiInstalledView -Manifest $m37 -Record $rec -Layout $j38
Check ($v.release -eq '0.7.216.110-tester.38' -and $v.apps.content_id -eq ('D' * 64) -and $v.apps.source -eq 'update' -and $v.data['profiles'].content_id -eq ('E' * 64)) 'view: an update record of this driver install is in force'
$v = Get-NagiInstalledView -Manifest $m37 -Record $rec -Layout ([pscustomobject]@{ kind = 'junction'; target_leaf = '0.7.216.109-tester.37' })
Check ($v.apps.content_id -eq ('B' * 64) -and $v.apps.source -eq 'install') 'view negative control: an apps record whose folder is not the junction target does not count'
$rec2 = $rec.PSObject.Copy(); $rec2.driver_content_id = ('F' * 64)
$v = Get-NagiInstalledView -Manifest $m37 -Record $rec2 -Layout $j38
Check ($v.release -eq '0.7.216.109-tester.37' -and $v.apps.source -eq 'install' -and $v.data.Count -eq 0) 'view negative control: a record of another driver install does not count'

# ---- 3. the manifest reader and the committed signed fixture ---------------------------------------------------------
$fixture = Join-Path $PSScriptRoot 'test-data\nagi-update'
$r = Read-NagiUpdateManifest (Join-Path $fixture 'nagi-update.json')
Check ($r.ok -and $r.manifest.release -eq '0.7.216.109-tester.37') "manifest: the fixture reads ($($r.reason))"
$base = Get-Content -LiteralPath (Join-Path $fixture 'nagi-update.json') -Raw
$broken = @(
    @{ n = 'another schema'; f = { param($t) $t.Replace('"nagi.update/1"', '"nagi.update/2"') } },
    @{ n = 'an asset with a folder'; f = { param($t) $t.Replace('"nagi-apps-0.7.216.109-tester.37.zip"', '"..\\x.zip"') } },
    @{ n = 'a release outside the grammar'; f = { param($t) $t.Replace('"release": "0.7.216.109-tester.37"', '"release": "latest"') } },
    @{ n = 'an apps file outside the pack'; f = { param($t) $t.Replace('"system-tuning/system-tuning.ps1"', '"../system-tuning.ps1"') } },
    @{ n = 'an unknown kind'; f = { param($t) $t.Replace('"kind": "apps"', '"kind": "plugin"') } },
    @{ n = 'no driver ABI'; f = { param($t) $t.Replace('"kmd_abi": "0x000700D8"', '"kmd_abi": "8"') } }
)
foreach ($b in $broken) {
    $p = Join-Path $run 'broken.json'
    $t = & $b.f $base
    Check ($t -ne $base) "manifest negative control $($b.n): the change applies"
    [IO.File]::WriteAllText($p, $t, (New-Object Text.UTF8Encoding $false))
    $r = Read-NagiUpdateManifest $p
    Check (-not $r.ok) "manifest negative control $($b.n): refused ($($r.reason))"
}
$signer = (Get-Content -LiteralPath (Join-Path $fixture 'signer.txt') -Raw).Trim()
$s = Test-NagiManifestSignature -ManifestPath (Join-Path $fixture 'nagi-update.json') -SignaturePath (Join-Path $fixture 'nagi-update.json.p7s') -Pinned @($signer)
Check $s.ok "signature: the fixture verifies with its pinned signer ($($s.reason))"
$t = [IO.File]::ReadAllBytes((Join-Path $fixture 'nagi-update.json')); $t[100] = [byte]($t[100] -bxor 0x20)
$p = Join-Path $run 'tampered.json'; [IO.File]::WriteAllBytes($p, $t)
$s = Test-NagiManifestSignature -ManifestPath $p -SignaturePath (Join-Path $fixture 'nagi-update.json.p7s') -Pinned @($signer)
Check (-not $s.ok) "signature negative control: one changed byte is refused ($($s.reason))"
$s = Test-NagiManifestSignature -ManifestPath (Join-Path $fixture 'nagi-update.json') -SignaturePath (Join-Path $fixture 'nagi-update.json.p7s')
Check (-not $s.ok) "signature negative control: the fixture's signer is not a release publisher ($($s.reason))"

# ---- 4. the atomic switch on real junctions --------------------------------------------------------------------------
function New-Root([string]$Name, [switch]$Classic) {
    $root = Join-Path $run ("$Name-" + [Guid]::NewGuid().ToString("N").Substring(0, 6))
    foreach ($v in 'v2', 'v3') {
        $d = Join-Path $root "apps\$v"; [void][IO.Directory]::CreateDirectory($d)
        [IO.File]::WriteAllText((Join-Path $d 'which.txt'), $v)
    }
    if ($Classic) {
        $c = Join-Path $root 'control'; [void][IO.Directory]::CreateDirectory($c)
        [IO.File]::WriteAllText((Join-Path $c 'which.txt'), 'v1')
    } else { New-NagiJunction (Join-Path $root 'control') (Join-Path $root 'apps\v2') }
    return $root
}
function Read-Which([string]$Root) {
    $f = Join-Path $Root 'control\which.txt'
    if (-not (Test-Path -LiteralPath $f)) { return '(none)' }
    return [IO.File]::ReadAllText($f)
}
function Test-Clean([string]$Root) {
    return -not (Test-Path -LiteralPath (Join-Path $Root 'control.new')) -and -not (Test-Path -LiteralPath (Join-Path $Root 'control.prev'))
}
function Invoke-SwitchCases {
    $bad = @()
    $root = New-Root 'classic' -Classic
    $r = Switch-NagiApps -InstallRoot $root -To 'v2' -ClassicName 'v1-installed'
    $l = Get-NagiAppsLayout $root
    if (-not ($r -eq 'switched' -and $l.kind -eq 'junction' -and $l.target_leaf -eq 'v2' -and (Read-Which $root) -eq 'v2' -and
        [IO.File]::ReadAllText((Join-Path $root 'apps\v1-installed\which.txt')) -eq 'v1' -and (Test-Clean $root))) { $bad += 'classic to junction' }
    $r = Switch-NagiApps -InstallRoot $root -To 'v3'
    if (-not ($r -eq 'switched' -and (Read-Which $root) -eq 'v3' -and [IO.File]::ReadAllText((Join-Path $root 'apps\v2\which.txt')) -eq 'v2' -and (Test-Clean $root))) { $bad += 'junction to junction' }
    $r = Switch-NagiApps -InstallRoot $root -To 'v3'
    if ($r -ne 'unchanged') { $bad += 'the same target again' }
    foreach ($step in @(@{ s = 'new'; want = 'v2'; how = 'undone' }, @{ s = 'moved'; want = 'v3'; how = 'finished' }, @{ s = 'switched'; want = 'v3'; how = 'finished' })) {
        $root = New-Root "stop-$($step.s)"
        try { Switch-NagiApps -InstallRoot $root -To 'v3' -StopAfter $step.s | Out-Null } catch { }
        $how = Repair-NagiAppsSwitch -InstallRoot $root
        $l = Get-NagiAppsLayout $root
        if (-not ($how -eq $step.how -and $l.kind -eq 'junction' -and (Read-Which $root) -eq $step.want -and (Test-Clean $root) -and
            [IO.File]::ReadAllText((Join-Path $root 'apps\v2\which.txt')) -eq 'v2' -and [IO.File]::ReadAllText((Join-Path $root 'apps\v3\which.txt')) -eq 'v3')) {
            $bad += "stop after $($step.s): repair $how, control $(Read-Which $root)"
        }
    }
    $root = New-Root 'stop-classic' -Classic
    try { Switch-NagiApps -InstallRoot $root -To 'v3' -ClassicName 'v1-installed' -StopAfter 'moved' | Out-Null } catch { }
    $how = Repair-NagiAppsSwitch -InstallRoot $root
    if (-not ($how -eq 'finished' -and (Read-Which $root) -eq 'v3' -and (Test-Clean $root))) { $bad += "classic stop after moved: repair $how, control $(Read-Which $root)" }
    return , $bad
}
$bad = Invoke-SwitchCases
Check ($bad.Count -eq 0) "switch: classic to junction, junction to junction, same target, a stop after new, moved and switched and its repair ($($bad -join '; '))"
$root = New-Root 'real-folder' -Classic
$m = $null; try { Remove-NagiJunction (Join-Path $root 'control') } catch { $m = $_.Exception.Message }
Check ($m -match 'real folder' -and (Read-Which $root) -eq 'v1') "switch: a real folder is never removed as a link ($m)"
$m = $null; try { Switch-NagiApps -InstallRoot $root -To 'v9' } catch { $m = $_.Exception.Message }
Check ($m -match 'not there' -and (Read-Which $root) -eq 'v1') 'switch: a target folder that is not there changes nothing'
$m = $null; try { Switch-NagiApps -InstallRoot $root -To 'v2' } catch { $m = $_.Exception.Message }
Check ($m -match 'needs a name' -and (Read-Which $root) -eq 'v1') 'switch: a classic folder without a name for its place changes nothing'
$switchMutants = @(
    @{ n = 'repair-without-forward'; o = "if (Test-NagiReparsePoint `$new) { [IO.Directory]::Move(`$new, `$control) }"; r = "if (`$false) { }" },
    @{ n = 'old-link-left'; o = "if (Test-Path -LiteralPath `$prev) { Remove-NagiJunction `$prev }"; r = '' }
)
foreach ($mu in $switchMutants) {
    $file = New-Mutant $mu.n $mu.o $mu.r
    $mbad = & { . $file; Invoke-SwitchCases }
    Check (@($mbad).Count -gt 0) "switch negative control $($mu.n): the cases catch it ($(@($mbad)[0]))"
}

# ---- 5. the apps pack ------------------------------------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
function New-Zip([string]$Path, [hashtable]$Entries) {
    $z = [IO.Compression.ZipFile]::Open($Path, 'Create')
    try { foreach ($k in ($Entries.Keys | Sort-Object)) { $e = $z.CreateEntry($k); $s = $e.Open(); $b = [Text.Encoding]::UTF8.GetBytes($Entries[$k]); $s.Write($b, 0, $b.Length); $s.Dispose() } }
    finally { $z.Dispose() }
}
function Sha([string]$Text) { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))) -replace '-', '') }
$files = @([pscustomobject]@{ path = 'a.txt'; sha256 = (Sha 'alpha') }, [pscustomobject]@{ path = 'sub/b.txt'; sha256 = (Sha 'beta') })
$zip = Join-Path $run 'good.zip'; New-Zip $zip @{ 'a.txt' = 'alpha'; 'sub/b.txt' = 'beta' }
$d = Join-Path $run 'pack-good'; Expand-NagiPack -Zip $zip -Destination $d -Files $files
Check ([IO.File]::ReadAllText((Join-Path $d 'sub\b.txt')) -eq 'beta') 'pack: every listed file is unpacked and checked'
foreach ($neg in @(@{ n = 'a file more'; e = @{ 'a.txt' = 'alpha'; 'sub/b.txt' = 'beta'; 'evil.dll' = 'x' } },
                   @{ n = 'a file less'; e = @{ 'a.txt' = 'alpha' } },
                   @{ n = 'a changed file'; e = @{ 'a.txt' = 'alpha'; 'sub/b.txt' = 'BETA' } })) {
    $zip = Join-Path $run "neg-$($neg.n -replace ' ', '-').zip"; New-Zip $zip $neg.e
    $d = Join-Path $run "pack-$($neg.n -replace ' ', '-')"
    $m = $null; try { Expand-NagiPack -Zip $zip -Destination $d -Files $files } catch { $m = $_.Exception.Message }
    Check ($m -and -not (Test-Path -LiteralPath $d)) "pack negative control $($neg.n): refused, no folder left ($m)"
}

# ---- 6. the transaction ----------------------------------------------------------------------------------------------
$old = 'A' * 64; $new = 'B' * 64; $other = 'C' * 64; $cat = 'c' * 64
function New-Txn([hashtable]$More = @{}) {
    $t = [pscustomobject]@{ schema = 'nagi.update-transaction/1'; id = 't1'; created_utc = '2026-11-04T10:00:00Z'; state = 'driver-pending'
        from = [pscustomobject]@{ release = '0.7.216.109-tester.37'; kmd_sha256 = $old; kmd_abi = '0x000700D8'; state_schema = 2; catalog_id = $cat; apps_folder = '0.7.216.109-tester.37' }
        to = [pscustomobject]@{ release = '0.7.216.110-tester.38'; kmd_sha256 = $new; kmd_abi = '0x000700D8'; state_schema = 2; catalog_id = $cat; apps_kmd_abis = @('0x000700D8') }
        apps_rollback = $true; boots = @(); decision = $null }
    foreach ($k in $More.Keys) { $t.$k = $More[$k] }
    return $t
}
function Boot($Kmd, $Confirm, $Verify) { return [pscustomobject]@{ boot = [Guid]::NewGuid().ToString('N'); kmd_sha256 = $Kmd; confirm = $Confirm; verify = $Verify } }
$now = [DateTime]::new(2026, 11, 5, 0, 0, 0, [DateTimeKind]::Utc)
$txnCases = @(
    @{ n = 'no boot yet'; b = @(); want = 'wait' },
    @{ n = 'the old KMD still runs (the install has not bound yet)'; b = @((Boot $old 0 'verified')); want = 'wait' },
    @{ n = 'a confirmed and verified start of the new KMD'; b = @((Boot $new 0 'verified')); want = 'commit' },
    @{ n = 'a start the guard reset after a healthy start (exit 6)'; b = @((Boot $new 6 'verified')); want = 'commit' },
    @{ n = 'confirmed, verify not run yet'; b = @((Boot $new 0 $null)); want = 'wait' },
    @{ n = 'one failed start'; b = @((Boot $new 1 $null)); want = 'wait' },
    @{ n = 'two failed starts'; b = @((Boot $new 1 $null), (Boot $new 3 'verify-failed')); want = 'rollback' },
    @{ n = 'two failed starts by verify alone'; b = @((Boot $new 0 'verify-failed'), (Boot $new $null 'verify-failed')); want = 'rollback' },
    # BD-359: a verification at a logon that failed gives no observation (''): two such boots neither count nor commit.
    @{ n = 'two confirmed starts whose logon verification failed (no observation) do not count'; b = @((Boot $new 0 ''), (Boot $new 0 '')); want = 'wait' },
    @{ n = 'two starts left to a kit (exit 2) do not count';b = @((Boot $new 2 $null), (Boot $new 2 $null)); want = 'wait' },
    @{ n = 'another KMD ran'; b = @((Boot $other 0 'verified')); want = 'supersede' },
    @{ n = 'two failed starts, another state catalog'; b = @((Boot $new 1 $null), (Boot $new 1 $null)); t = @{ to = [pscustomobject]@{ release = 'x'; kmd_sha256 = $new; kmd_abi = '0x000700D8'; state_schema = 2; catalog_id = ('d' * 64); apps_kmd_abis = @('0x000700D8') } }; want = 'manual' },
    @{ n = 'two failed starts, another state schema'; b = @((Boot $new 1 $null), (Boot $new 1 $null)); t = @{ to = [pscustomobject]@{ release = 'x'; kmd_sha256 = $new; kmd_abi = '0x000700D8'; state_schema = 3; catalog_id = $cat; apps_kmd_abis = @('0x000700D8') } }; want = 'manual' },
    @{ n = 'two failed starts, apps that cannot go back and do not speak the old ABI'; b = @((Boot $new 1 $null), (Boot $new 1 $null)); t = @{ apps_rollback = $false; to = [pscustomobject]@{ release = 'x'; kmd_sha256 = $new; kmd_abi = '0x000700D9'; state_schema = 2; catalog_id = $cat; apps_kmd_abis = @('0x000700D9') } }; want = 'manual' },
    @{ n = 'two failed starts, apps that cannot go back but speak the old ABI'; b = @((Boot $new 1 $null), (Boot $new 1 $null)); t = @{ apps_rollback = $false }; want = 'rollback' },
    @{ n = 'no decision after the time bound'; b = @((Boot $new 1 $null)); t = @{ created_utc = '2026-10-20T00:00:00Z' }; want = 'expire' },
    @{ n = 'a transaction that is not pending'; b = @((Boot $new 1 $null), (Boot $new 1 $null)); t = @{ state = 'committed' }; want = 'none' }
)
function Invoke-TxnCases {
    $bad = @()
    foreach ($c in $txnCases) {
        $more = $(if ($c.t) { $c.t } else { @{} })
        $t = New-Txn $more; $t.boots = @($c.b)
        $d = Get-NagiTxnDecision -Txn $t -Now $now
        if ($d -ne $c.want) { $bad += "$($c.n): $d" }
    }
    return , $bad
}
$bad = Invoke-TxnCases
foreach ($c in $txnCases) { Check (-not ($bad | Where-Object { $_.StartsWith("$($c.n):") })) "transaction: $($c.n) -> $($c.want)" }
$txnMutants = @(
    @{ n = 'failures-not-counted'; o = 'if ($bad) { $failed++ }'; r = '' },
    @{ n = 'no-catalog-rule'; o = "([string]`$Txn.from.catalog_id -eq [string]`$Txn.to.catalog_id) -and"; r = '' },
    @{ n = 'commit-without-verify'; o = "-and ([string]`$b.verify -eq 'verified')"; r = '' }
)
foreach ($mu in $txnMutants) {
    $file = New-Mutant $mu.n $mu.o $mu.r
    $mbad = & { . $file; Invoke-TxnCases }
    Check (@($mbad).Count -gt 0) "transaction negative control $($mu.n): the cases catch it ($(@($mbad)[0]))"
}
# One merge per boot.
$t = New-Txn
$t = Add-NagiTxnObservation -Txn $t -Boot '7' -KmdSha256 $new -Confirm 0
$t = Add-NagiTxnObservation -Txn $t -Boot '7' -Verify 'verified'
Check (@($t.boots).Count -eq 1 -and $t.boots[0].confirm -eq 0 -and $t.boots[0].verify -eq 'verified' -and $t.boots[0].kmd_sha256 -eq $new) 'transaction: the confirmation and the verify of one boot merge into one record'
# The step end to end, with stand-in actions.
$ur = Join-Path $run 'update-root'
Save-NagiTransaction (New-Txn) $ur
$calls = New-Object Collections.ArrayList
$rb = { param($x) [void]$calls.Add("driver $($x.id)"); [pscustomobject]@{ ok = $true; detail = 'stand-in rollback' } }.GetNewClosure()
$sa = { param($f) [void]$calls.Add("apps $f"); 'switched' }.GetNewClosure()
$l1 = Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '1' -KmdSha256 $new -Confirm 1 -RollbackDriver $rb -SwitchApps $sa -Now $now
$l2 = Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '2' -KmdSha256 $new -Confirm 5 -RollbackDriver $rb -SwitchApps $sa -Now $now
$t = Read-NagiTransaction $ur
Check ($l1 -match 'wait' -and $l2 -match 'rollback -> rollback-pending' -and $t.state -eq 'rollback-pending' -and ($calls -join ';') -eq 'apps 0.7.216.109-tester.37;driver t1') "step: two failed starts run the apps switch and the driver rollback ($l1 | $l2 | $($calls -join ';'))"
$l3 = Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '3' -KmdSha256 $old -Confirm 0 -Now $now
Check ($l3 -match 'rolled-back' -and (Read-NagiTransaction $ur).state -eq 'rolled-back') "step: the next start of the old KMD closes it as rolled back ($l3)"
Save-NagiTransaction (New-Txn) $ur
$rollbackFails = { param($x) [pscustomobject]@{ ok = $false; detail = 'DiRollbackDriver 0xE0000219' } }
Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '1' -KmdSha256 $new -Confirm 1 -RollbackDriver $rollbackFails -Now $now | Out-Null
$l = Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '2' -KmdSha256 $new -Confirm 1 -RollbackDriver $rollbackFails -Now $now
Check ($l -match '-> manual' -and (Read-NagiTransaction $ur).decision -match '0xE0000219') "step: a rollback that Windows refuses leaves the notice for the user ($l)"
Save-NagiTransaction (New-Txn) $ur
$l = Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '1' -KmdSha256 $new -Confirm 0 -Verify 'verified' -Now $now
Check ($l -match 'commit -> committed') "step: a good start commits ($l)"
Check ((Invoke-NagiUpdateConfirmStep -UpdateRoot (Join-Path $run 'nothing') -Boot '1') -eq 'update transaction: none') 'step: no transaction, nothing to do'
[IO.File]::WriteAllText((Join-Path $ur 'transaction.json'), '{ broken')
Check ((Invoke-NagiUpdateConfirmStep -UpdateRoot $ur -Boot '1') -eq 'update transaction: none') 'step: an unreadable transaction is no transaction and no exception'

# ---- 7. the pinned signers -------------------------------------------------------------------------------------------
$cs = Join-Path (Split-Path $PSScriptRoot) 'win\amdgpu_wddm_control\src\SelfUpdate.cs'
$csText = [IO.File]::ReadAllText($cs)
$block = [regex]::Match($csText, 'PinnedSigners\s*=\s*\{([^}]*)\}').Groups[1].Value
$csList = @([regex]::Matches($block, '"([0-9A-F]{40})"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
$psList = @($script:NagiPinnedSigners | Sort-Object)
Check ($csList.Count -gt 0 -and ($csList -join ',') -eq ($psList -join ',')) "pinned signers: SelfUpdate.cs and nagi-update.ps1 hold one list ($($csList -join ',') / $($psList -join ','))"

Remove-Item -LiteralPath $run -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "nagi-update: $script:pass passed, $script:fail failed"
if ($script:fail) { exit 1 }
exit 0
