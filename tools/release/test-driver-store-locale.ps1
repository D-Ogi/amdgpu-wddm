# Pure host regression checks. All Windows inventory and native calls are mocked.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$Out = '')
$ErrorActionPreference = 'Stop'
# The parent dry-run suite may supply its own inventory. This test owns its fake
# Windows provider and fixture inputs; do not let that process-only override leak in.
$inheritedDriverStore = [Environment]::GetEnvironmentVariable('AMDGPU_WDDM_TEST_DRIVER_STORE', 'Process')
[Environment]::SetEnvironmentVariable('AMDGPU_WDDM_TEST_DRIVER_STORE', $null, 'Process')
try {
. (Join-Path $Installer 'common.ps1')
$fail = 0; $checks = 0
function Check([bool]$Ok, [string]$Text) {
    $script:checks++
    if ($Ok) { "PASS $Text" } else { $script:fail++; "FAIL $Text" }
}
# Synthetic examples, NOT captured Windows output. Escapes keep this PS5.1 script ASCII.
$fixtures = '[
 {"tag":"en-US","published":"Published Name","original":"Original Name","version":"Driver Version","error":"Access denied"},
 {"tag":"pl-PL","published":"Opublikowana nazwa","original":"Oryginalna nazwa","version":"Wersja sterownika","error":"Odmowa dost\u0119pu"},
 {"tag":"es-ES","published":"Nombre publicado","original":"Nombre original","version":"Versi\u00f3n del controlador","error":"Acceso denegado"},
 {"tag":"ja-JP","published":"\u516c\u958b\u540d","original":"\u5143\u306e\u540d\u524d","version":"\u30c9\u30e9\u30a4\u30d0\u30fc \u30d0\u30fc\u30b8\u30e7\u30f3","error":"\u30a2\u30af\u30bb\u30b9\u304c\u62d2\u5426\u3055\u308c\u307e\u3057\u305f"},
 {"tag":"ko-KR","published":"\uac8c\uc2dc\ub41c \uc774\ub984","original":"\uc6d0\ub798 \uc774\ub984","version":"\ub4dc\ub77c\uc774\ubc84 \ubc84\uc804","error":"\uc561\uc138\uc2a4\uac00 \uac70\ubd80\ub418\uc5c8\uc2b5\ub2c8\ub2e4"}
]' | ConvertFrom-Json
$script:fixtureRows = @(
    [pscustomobject]@{ Driver='oem9.inf'; OriginalFileName='C:\Windows\System32\DriverStore\FileRepository\bc250kmd.inf_x\bc250kmd.inf'; Version=[version]'0.7.198.100' }
    [pscustomobject]@{ Driver='oem12.inf'; OriginalFileName='bc250kmd.inf'; Version=[version]'0.7.208.100' }
    [pscustomobject]@{ Driver='oem13.inf'; OriginalFileName='foreign.inf'; Version=[version]'1.2.3.4' }
    [pscustomobject]@{ Driver='OEM15.INF'; OriginalFileName='BC250KMD.INF'; Version=[version]'0.7.213.101' })
$script:originalRows = @($script:fixtureRows)
$script:inventoryCalls = 0; $script:inventoryFailAt = 0; $script:driverMode = $false
function Get-WindowsDriver { param([switch]$Online, $ErrorAction)
    if (-not $Online -or $ErrorAction -ne 'Stop') { throw 'inventory must use -Online -ErrorAction Stop' }
    $script:inventoryCalls++
    if ($script:inventoryFailAt -eq $script:inventoryCalls) { throw 'Synthetic inventory failure' }
    if ($script:inventoryError) { throw $script:inventoryError }
    return $script:fixtureRows
}
function Invoke-Native { param($File, $Arguments)
    $script:nativeCalls++
    if ($script:driverMode) {
        if ($File -eq 'pnputil.exe' -and $Arguments[0] -eq '/delete-driver') {
            $script:deleted += $Arguments[1]
            Check (($Arguments.Count -eq 4) -and $Arguments[2] -eq '/uninstall' -and $Arguments[3] -eq '/force') 'actual uninstall delete arguments' | Out-Null
            $code = 0
            if ($script:deleteCodes.ContainsKey($Arguments[1])) { $code = $script:deleteCodes[$Arguments[1]] }
            if ($code -in @(0, 3010) -and $script:retain -notcontains $Arguments[1]) {
                $script:fixtureRows = @($script:fixtureRows | Where-Object { $_.Driver -ne $Arguments[1] })
            }
            return @{ code = $code; text = $script:translated }
        }
        if ($File -eq 'pnputil.exe' -and $Arguments[0] -eq '/scan-devices') { $script:scans++; return @{code=0;text=''} }
        if ($File -eq 'sc.exe' -and $Arguments[0] -eq 'delete' -and $Arguments[1] -eq 'bc250kmd') { $script:servicesRemoved++; return @{code=0;text=''} }
        throw 'Unexpected fake native invocation'
    }
    return @{code=$(if ($script:inventoryError) {5} else {0});text=$(if ($script:inventoryError) {$script:inventoryError} else {$script:translated})}
}
# Test the real dry-run fixture reader too. PS5.1 can preserve an empty JSON array
# as one nested object when ConvertFrom-Json is returned directly inside @(...).
function Get-Content { param($LiteralPath, [switch]$Raw, $ErrorAction)
    if ($LiteralPath -cne 'fixture:driver-store.json' -or -not $Raw -or $ErrorAction -ne 'Stop') { throw 'Unexpected fake content read' }
    $script:fixtureReads++
    return $script:fixtureJson
}
$script:DryRunMode = $true; $script:fixtureReads = 0
$env:AMDGPU_WDDM_TEST_DRIVER_STORE = 'fixture:driver-store.json'
try {
    $script:fixtureJson = '[]'
    Check (@(Read-DriverStorePackages).Count -eq 0) 'empty JSON fixture enumerates zero packages under PS5.1'
    Check (@(Get-OurDriverPackageList).Count -eq 0) 'empty JSON fixture maps to an empty driver list'
    $script:fixtureJson = '[{"Driver":"oem9.inf","OriginalFileName":"bc250kmd.inf","Version":"0.7.198.100"}]'
    $one = @(Get-OurDriverPackageList)
    Check ($one.Count -eq 1 -and $one[0].published -ceq 'oem9.inf' -and $one[0].version -ceq '0.7.198.100') 'one JSON package keeps its identity and version'
    $script:DryRunMode = $false
    $rows = @(Read-DriverStorePackages)
    Check ($rows.Count -eq 4 -and $script:fixtureReads -eq 3 -and $script:inventoryCalls -eq 1) 'non-dry provider ignores the fixture override and uses mocked Windows'
} finally {
    [Environment]::SetEnvironmentVariable('AMDGPU_WDDM_TEST_DRIVER_STORE', $null, 'Process')
    $script:DryRunMode = $false
}
# Exact previous parser from be8c9ae1, renamed as a negative control. The English
# fixture must first return all three packages before a translated fixture can prove regression.
function Get-LegacyDriverPackageRows([string]$Text) {
    $blocks = $Text -split "(\r?\n){2,}"
    $r = @()
    foreach ($b in $blocks) {
        if ($b -notmatch '(?im)^\s*Original Name:\s*bc250kmd\.inf\s*$') { continue }
        $published = $null
        if ($b -match '(?im)^\s*Published Name:\s*(oem\d+\.inf)\s*$') { $published = $Matches[1] }
        if (-not $published) { continue }
        $version = $null
        if ($b -match '(?im)^\s*Driver Version:\s*\S+\s+(\d+(?:\.\d+){1,3})\s*$') { $version = $Matches[1] }
        $r += [pscustomobject]@{ published = $published; version = $version }
    }
    return $r
}

# Run the actual uninstall gates without dot-sourcing the executable uninstaller.
# Every exit in the selected AST is replaced with a test-only sentinel exception;
# no production expression, condition, or removal command is rewritten.
$tokens = $null; $parseErrors = $null
$uninstallText = [IO.File]::ReadAllText((Join-Path $Installer 'uninstall.ps1'))
$uninstallAst = [Management.Automation.Language.Parser]::ParseInput($uninstallText, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'uninstall.ps1 does not parse' }
function Test-UninstallBlock([int]$Start, [int]$End) {
    $text = $uninstallText.Substring($Start, $End - $Start)
    $exits = @($uninstallAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.ExitStatementAst] -and
        $node.Extent.StartOffset -ge $Start -and $node.Extent.EndOffset -le $End
    }.GetNewClosure(), $true) | Sort-Object { $_.Extent.StartOffset } -Descending)
    foreach ($node in $exits) {
        $code = $node.Pipeline.Extent.Text
        if ($code -notmatch '^\d+$') { throw 'Unexpected nonliteral uninstall exit' }
        $at = $node.Extent.StartOffset - $Start
        $text = $text.Remove($at, $node.Extent.EndOffset - $node.Extent.StartOffset).Insert($at, "throw 'fixture-uninstall-exit:$code'")
    }
    return [scriptblock]::Create($text)
}
$statements = @($uninstallAst.EndBlock.Statements)
$inventory = @($statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] -and $_.Extent.Text -like 'try { $pkgs = @(Get-OurDriverPackages)*' })
$firstChange = @($uninstallAst.FindAll({ param($n) $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Invoke-Change' }, $true) | Sort-Object { $_.Extent.StartOffset })[0]
Check ($inventory.Count -eq 1 -and $inventory[0].Extent.EndOffset -lt $firstChange.Extent.StartOffset) 'inventory precedes the first uninstall mutation'
$inventoryBlock = Test-UninstallBlock $inventory[0].Extent.StartOffset $inventory[0].Extent.EndOffset
$startDelete = @($statements | Where-Object { $_.Extent.Text -eq '$packageRestart = $false' })
$service = @($statements | Where-Object { $_.Extent.Text -like 'Invoke-Change "remove the bc250kmd service entry*' })
$files = @($statements | Where-Object { $_.Extent.Text -eq 'Invoke-Change "remove $root" { Remove-PathOrSchedule $root } | Out-Null' })
Check ($startDelete.Count -eq 1 -and $service.Count -eq 1 -and $files.Count -eq 1) 'actual package, service and file statements selected'
$driverBlock = Test-UninstallBlock $startDelete[0].Extent.StartOffset $service[0].Extent.EndOffset
$fileBlock = Test-UninstallBlock $files[0].Extent.StartOffset $files[0].Extent.EndOffset
$startFootprint = @($statements | Where-Object { $_.Extent.Text -like '$footprint = @(Get-ReleaseFootprint *' })
Check ($startFootprint.Count -eq 1) 'actual final footprint branch selected'
$footprintBlock = Test-UninstallBlock $startFootprint[0].Extent.StartOffset $uninstallAst.EndBlock.Extent.EndOffset

function Write-Info { param($Text) }
function Write-Log { param($Text) }
function Write-Fail { param($Text) $script:reportedFailures += $Text }
function Write-Warn2 { param($Text) $script:reportedWarnings += $Text }
function Write-Host { param($Object, $ForegroundColor) }
function Get-Service { param($Name, $ErrorAction) return [pscustomobject]@{Name=$Name} }
function Test-Path { param($LiteralPath) return $false }
function Remove-Item { throw 'Unexpected direct filesystem operation' }
function Remove-PathOrSchedule { param($Path) $script:filesRemoved += $Path }
function Get-MftRegistrationKeysPresent { param($ClassesKey) return @() }
function Read-Confirmation { throw 'No interactive prompt is allowed in this test' }
function Get-ReleaseFootprint { param($InstallRoot, $State, $MftKeys) return $script:footprintRows }
function Reset-UninstallFixture {
    $script:fixtureRows = @($script:originalRows)
    $script:inventoryError = $null; $script:inventoryFailAt = 0; $script:inventoryCalls = 0
    $script:driverMode = $true; $script:DryRunMode = $false; $script:Mutated = $false
    $script:deleteCodes = @{}; $script:retain = @(); $script:deleted = @(); $script:nativeCalls = 0
    $script:servicesRemoved = 0; $script:filesRemoved = @(); $script:scans = 0
    $script:reportedFailures = @(); $script:reportedWarnings = @()
}
function Run-UninstallFixture {
    param([switch]$Dry)
    $DryRun = [bool]$Dry; $script:DryRunMode = [bool]$Dry; $root = 'Q:\fixture-driver-root'
    try {
        . $inventoryBlock
        . $driverBlock
        . $fileBlock
        return 0
    } catch {
        if ($_.Exception.Message -match '^fixture-uninstall-exit:(\d+)$') { return [int]$Matches[1] }
        throw
    }
}
function Run-FootprintFixture {
    param([object[]]$Rows, [switch]$Dry)
    $script:footprintRows = $Rows; $DryRun = [bool]$Dry; $NoReboot = $true
    $root = 'Q:\fixture-driver-root'; $state = $null
    try { . $footprintBlock; throw 'Missing explicit uninstall exit' }
    catch {
        if ($_.Exception.Message -match '^fixture-uninstall-exit:(\d+)$') { return [int]$Matches[1] }
        throw
    }
}
$oldCulture = [Threading.Thread]::CurrentThread.CurrentCulture
$oldUi = [Threading.Thread]::CurrentThread.CurrentUICulture
try {
    foreach ($f in $fixtures) {
        [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo($f.tag)
        [Threading.Thread]::CurrentThread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo($f.tag)
        $script:driverMode = $false; $script:fixtureRows = @($script:originalRows)
        $script:inventoryFailAt = 0; $script:inventoryCalls = 0
        $script:translated = (@($script:fixtureRows | ForEach-Object {
            "$($f.published): $($_.Driver)`r`n$($f.original): $([IO.Path]::GetFileName($_.OriginalFileName))`r`n$($f.version): 10/02/2026 $($_.Version)"
        }) -join "`r`n`r`n")
        $script:inventoryError = $null; $script:nativeCalls = 0
        $legacy = @(Get-LegacyDriverPackageRows $script:translated)
        Check ($legacy.Count -eq $(if ($f.tag -eq 'en-US') { 3 } else { 0 })) "$($f.tag): old parser English positive control / translated regression"
        if ($f.tag -eq 'en-US') {
            Check (($legacy.published -join ',').ToLowerInvariant() -ceq 'oem9.inf,oem12.inf,oem15.inf') 'old English parser identifies all three expected packages'
            Check (($legacy.version -join ',') -ceq '0.7.198.100,0.7.208.100,0.7.213.101') 'old English parser versions match structured fixtures'
        }
        $rows = @(Get-OurDriverPackageList)
        Check (($rows.Count -eq 3) -and (($rows.published -join ',') -eq 'oem9.inf,oem12.inf,oem15.inf')) "$($f.tag): bound and staged packages, foreign package excluded"
        Check ($script:nativeCalls -eq 0) "$($f.tag): no localized native listing is used"
        $plan = Get-DriverStoreRemovePlan -Packages $rows -BoundPublished 'oem15.inf' -PreviousVersion '0.7.208.100'
        Check (($plan.remove -join ',') -eq 'oem9.inf') "$($f.tag): bound and rollback packages retained"
        $script:inventoryError = $f.error
        $threw = $false; try { [void](Get-OurDriverPackageList) } catch { $threw = $true }
        Check $threw "$($f.tag): failed inventory is not empty success"
        $reading = Read-ReleaseFootprintItem 'driver store' { throw $script:inventoryError }
        Check (-not $reading.known) "$($f.tag): unknown footprint stays unknown"

        Reset-UninstallFixture; $script:inventoryFailAt = 1
        Check ((Run-UninstallFixture) -eq 6 -and -not $script:Mutated -and $script:nativeCalls -eq 0) "$($f.tag): inventory error stops before all changes"
        Reset-UninstallFixture; $script:deleteCodes['oem9.inf'] = 5
        Check ((Run-UninstallFixture) -eq 6 -and $script:deleted.Count -eq 1 -and $script:servicesRemoved -eq 0 -and $script:filesRemoved.Count -eq 0) "$($f.tag): delete error retains service and files"
        Reset-UninstallFixture; $script:deleteCodes['oem12.inf'] = 5
        Check ((Run-UninstallFixture) -eq 6 -and ($script:deleted -join ',') -ceq 'oem9.inf,oem12.inf' -and $script:servicesRemoved -eq 0 -and $script:filesRemoved.Count -eq 0) "$($f.tag): partial deletion stops at the first failed package"
        Reset-UninstallFixture; $script:inventoryFailAt = 2
        Check ((Run-UninstallFixture) -eq 6 -and $script:servicesRemoved -eq 0 -and $script:filesRemoved.Count -eq 0 -and $script:scans -eq 0) "$($f.tag): failed re-enumeration retains service and files"
        Reset-UninstallFixture; $script:retain = @('oem9.inf')
        Check ((Run-UninstallFixture) -eq 6 -and $script:servicesRemoved -eq 0 -and $script:filesRemoved.Count -eq 0) "$($f.tag): remaining package prevents destructive continuation"
        Reset-UninstallFixture; $script:retain = @('oem9.inf'); $script:deleteCodes['oem9.inf'] = 3010
        Check ((Run-UninstallFixture) -eq 3010 -and $script:servicesRemoved -eq 0 -and $script:filesRemoved.Count -eq 0) "$($f.tag): remaining package with 3010 requests restart and preserves dependencies"
        Reset-UninstallFixture; $script:deleteCodes['oem9.inf'] = 3010
        Check ((Run-UninstallFixture) -eq 0 -and $script:servicesRemoved -eq 1 -and $script:filesRemoved.Count -eq 1 -and $script:scans -eq 1) "$($f.tag): 3010 with verified empty store permits final cleanup"
        Reset-UninstallFixture
        Check ((Run-UninstallFixture) -eq 0 -and $script:servicesRemoved -eq 1 -and $script:filesRemoved.Count -eq 1) "$($f.tag): successful deletion permits cleanup"
        Reset-UninstallFixture; $script:fixtureRows = @($script:originalRows[2])
        Check ((Run-UninstallFixture) -eq 0 -and $script:deleted.Count -eq 0 -and $script:servicesRemoved -eq 1) "$($f.tag): foreign package does not block cleanup of orphan files"
        Reset-UninstallFixture
        Check ((Run-UninstallFixture -Dry) -eq 0 -and $script:nativeCalls -eq 0 -and $script:filesRemoved.Count -eq 0 -and $script:inventoryCalls -eq 1) "$($f.tag): dry run uses inventory but makes no changes"
        $unknownRow = [pscustomobject]@{item='driver store';known=$false;present=$false;kept=$false;detail='not read'}
        $remainingRow = [pscustomobject]@{item='install root';known=$true;present=$true;kept=$false;detail='pending restart'}
        $goneRow = [pscustomobject]@{item='driver store';known=$true;present=$false;kept=$false;detail='gone'}
        $keptRow = [pscustomobject]@{item='control application data';known=$true;present=$true;kept=$true;detail='kept'}
        Check ((Run-FootprintFixture @($unknownRow)) -eq 6) "$($f.tag): unknown final footprint is not uninstall success"
        Check ((Run-FootprintFixture @($remainingRow)) -eq 0) "$($f.tag): known deferred file reports restart path"
        Check ((Run-FootprintFixture @($goneRow, $keptRow)) -eq 0) "$($f.tag): gone plus intentionally kept is success"
        Check ((Run-FootprintFixture @($unknownRow) -Dry) -eq 0) "$($f.tag): dry-run unknown report is allowed"
    }
} finally {
    [Threading.Thread]::CurrentThread.CurrentCulture = $oldCulture
    [Threading.Thread]::CurrentThread.CurrentUICulture = $oldUi
}
$script:driverMode = $false; $script:fixtureRows = @($script:originalRows)
Check (@(Get-OurDriverPackageRows -Packages @()).Count -eq 0) 'successful empty inventory is valid'
foreach ($bad in @(
    [pscustomobject]@{Driver='oem7.inf';Version='1.2.3.4'},
    [pscustomobject]@{Driver='..\oem7.inf';OriginalFileName='bc250kmd.inf';Version='1.2.3.4'},
    [pscustomobject]@{Driver='oem7.inf';OriginalFileName='bc250kmd.inf';Version='unknown'})) {
    $threw = $false; try { [void](Get-OurDriverPackageRows -Packages @($bad)) } catch { $threw = $true }
    Check $threw 'incomplete or malformed inventory refused'
}
$threw = $false
try { [void](Get-OurDriverPackageRows -Packages @($script:fixtureRows[0], $script:fixtureRows[0])) } catch { $threw=$true }
Check $threw 'duplicate published identity refused'
$row = Read-ReleaseFootprintItem 'driver store' { @{present=$false;detail='empty'} }
Check ($row.known -and -not $row.present) 'readable absence differs from failed read'
$row = Read-ReleaseFootprintItem 'driver store' { @{present=$true;detail='oem9.inf'} }
Check ($row.known -and $row.present) 'remaining package is present'
"$checks checks, $fail failures"
if ($Out) {
    [void][IO.Directory]::CreateDirectory($Out)
    $report = [ordered]@{ schema=1;ok=($fail -eq 0);checks=$checks;failures=$fail;cultures=@($fixtures.tag);samples='synthetic';osChanges=0;uninstallGates='actual AST with stubbed native and filesystem calls' }
    [IO.File]::WriteAllText((Join-Path $Out 'driver-store-locale-tests.json'), ($report | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
}
if ($fail) { exit 1 }
} finally {
    [Environment]::SetEnvironmentVariable('AMDGPU_WDDM_TEST_DRIVER_STORE', $inheritedDriverStore, 'Process')
}
