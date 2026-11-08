# The lab half of the swap check of a KMD image (tools/state/kmd_swap_catalog.py is the host half; README.md here).
# A kit runs this before its first change on the lab (selector, push, UMD or KMD swap). swap-sys.ps1 runs the same
# check right before the copy. It refuses when the installed state catalog is not the catalog that the candidate
# compiles:
#   - Parameters\DefaultsV2: Schema and CatalogId, written by the INF of the installed package. DriverStateInit
#     (driver/kmd/state_kernel.c) refuses DriverEntry with 0xC0000365 on another CatalogId.
#   - SharedState\StateV2: the generation that Active names has a CatalogId. A generation of another catalog is the
#     first start of a package install that has not run yet (a pending handoff), or a damaged store. A KMD-only swap
#     must not take either.
# Read only. Windows PowerShell 5.1. Exit 0 CATALOG MATCH, 5 REFUSED (mismatch), 2 usage or read error.
#   target.py ps catalog-precheck.ps1 -Expected <64 hex digits> [-Schema 2]
# target.py ps sends one file, so swap-sys.ps1 holds a copy of the block between the two marker lines; the host test
# test-catalog-precheck.ps1 fails when the two copies differ.
param(
    [Parameter(Mandatory = $true)][string]$Expected,
    [int]$Schema = 2,
    # Tests point this at a key of their own (test-catalog-precheck.ps1). The lab uses the default.
    [string]$ServiceKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd'
)
$ErrorActionPreference = 'Stop'
# >>> state catalog check (kmd-swap)
function Test-StateCatalog([string]$Expected, [int]$Schema, [string]$ServiceKey) {
    # Returns 0 (match), 5 (refused) or 2 (usage or read error), and writes its lines to the output.
    function Value([string]$Key, [string]$Name) {
        if (-not (Test-Path -LiteralPath $Key)) { return $null }
        $p = Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction SilentlyContinue
        if ($null -eq $p) { return $null }
        return $p.$Name
    }
    $want = "$Expected".Trim().ToLowerInvariant()
    if ($want -notmatch '^[0-9a-f]{64}$') { Write-Output "ERROR: the expected catalog is not 64 hex digits: '$Expected'"; return 2 }
    try {
        $defaults = Join-Path $ServiceKey 'Parameters\DefaultsV2'
        $dSchema = Value $defaults 'Schema'
        $dCatalog = Value $defaults 'CatalogId'
        $state = Join-Path $ServiceKey 'SharedState\StateV2'
        $active = Value $state 'Active'
        $gCatalog = $null
        if ($active) { $gCatalog = Value (Join-Path $state "Generations\$active") 'CatalogId' }
    } catch { Write-Output "ERROR: registry read failed: $($_.Exception.Message)"; return 2 }
    Write-Output "expected   $want schema $Schema"
    Write-Output "DefaultsV2 $dCatalog schema $dSchema"
    if ($active) { Write-Output "StateV2    $gCatalog generation $active" } else { Write-Output 'StateV2    no active generation' }
    $why = @()
    if ($null -eq $dCatalog) { $why += 'DefaultsV2 has no CatalogId (no installed shared-state package)' }
    elseif ("$dCatalog".ToLowerInvariant() -ne $want) { $why += "DefaultsV2 CatalogId is $dCatalog" }
    if ($null -eq $dSchema -or [int]$dSchema -ne $Schema) { $why += "DefaultsV2 Schema is $dSchema, not $Schema" }
    if ($active) {
        if ($null -eq $gCatalog) { $why += "the active generation $active has no CatalogId" }
        elseif ("$gCatalog".ToLowerInvariant() -ne $want) {
            $why += "the active generation $active has the catalog $gCatalog (a package install whose first start did not run, or a damaged store)"
        }
    }
    if ($why.Count) {
        foreach ($w in $why) { Write-Output "REFUSED: $w" }
        Write-Output 'REFUSED: a KMD-only swap of this candidate cannot start here. Build it on the installed catalog, or install a package of it.'
        return 5
    }
    Write-Output "CATALOG MATCH $want"
    return 0
}
# <<< state catalog check (kmd-swap)
$lines = @(Test-StateCatalog -Expected $Expected -Schema $Schema -ServiceKey $ServiceKey)
$code = [int]$lines[-1]
if ($lines.Count -gt 1) { $lines[0..($lines.Count - 2)] }
exit $code
