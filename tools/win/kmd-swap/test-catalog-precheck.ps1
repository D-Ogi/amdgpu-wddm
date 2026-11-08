# Host test of catalog-precheck.ps1: each case builds a service key under HKCU, runs the check in Windows PowerShell
# 5.1 (the lab shell) and compares the exit code and one line of the output. The key is removed at the end.
#   powershell -NoProfile -File tools\win\kmd-swap\test-catalog-precheck.ps1
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'catalog-precheck.ps1'
$base = "HKCU:\Software\bc250-test-catalog-precheck-$PID"
$a = 'b9c5c1b8d559f5ff16e110bb67508d503a6a6f60f2caf2c0769fbd15f84ac292'
$b = '02c7eed429a46a20825918b23c7adc364f5748f57bacc97b342bfac60d961860'
$fail = 0; $n = 0

function Service([string]$Name, $Defaults, [int]$Schema = 2, [string]$Active, $Generation) {
    $key = Join-Path $base $Name
    New-Item -Path (Join-Path $key 'Parameters\DefaultsV2') -Force | Out-Null
    if ($null -ne $Defaults) { New-ItemProperty -Path (Join-Path $key 'Parameters\DefaultsV2') -Name CatalogId -Value $Defaults -PropertyType String | Out-Null }
    New-ItemProperty -Path (Join-Path $key 'Parameters\DefaultsV2') -Name Schema -Value $Schema -PropertyType DWord | Out-Null
    if ($Active) {
        $g = Join-Path $key "SharedState\StateV2\Generations\$Active"
        New-Item -Path $g -Force | Out-Null
        New-ItemProperty -Path (Join-Path $key 'SharedState\StateV2') -Name Active -Value $Active -PropertyType String | Out-Null
        if ($null -ne $Generation) { New-ItemProperty -Path $g -Name CatalogId -Value $Generation -PropertyType String | Out-Null }
    }
    return $key
}
function Case([string]$Name, [string]$Key, [string]$Expected, [int]$Code, [string]$Line) {
    $script:n++
    $out = & powershell.exe -NoProfile -NonInteractive -File $script -Expected $Expected -ServiceKey $Key 2>&1 | Out-String
    $got = $LASTEXITCODE
    if ($got -ne $Code -or $out -notlike "*$Line*") {
        $script:fail++
        "FAIL $Name : exit $got (want $Code), want line '$Line'"
        $out
    } else { "ok   $Name : exit $got" }
}

try {
    $g1 = '{11111111-2222-3333-4444-555555555555}'
    Case 'match, no generation' (Service 'm0' $a) $a 0 "CATALOG MATCH $a"
    Case 'match with generation' (Service 'm1' $a 2 $g1 $a) $a 0 "CATALOG MATCH $a"
    Case 'expected in upper case' (Service 'm2' $a 2 $g1 $a) $a.ToUpperInvariant() 0 'CATALOG MATCH'
    Case 'A1 s3: installed rc1, candidate 02c7' (Service 'x1' $a 2 $g1 $a) $b 5 "REFUSED: DefaultsV2 CatalogId is $a"
    Case 'pending handoff: generation of the old catalog' (Service 'x2' $b 2 $g1 $a) $b 5 "the active generation $g1 has the catalog $a"
    Case 'generation without CatalogId' (Service 'x3' $a 2 $g1 $null) $a 5 'has no CatalogId'
    Case 'no DefaultsV2 CatalogId' (Service 'x4' $null) $a 5 'no installed shared-state package'
    Case 'schema 1' (Service 'x5' $a 1) $a 5 'Schema is 1, not 2'
    Case 'no service key' (Join-Path $base 'absent') $a 5 'REFUSED: DefaultsV2 has no CatalogId'
    Case 'expected not an id' (Service 'u1' $a) 'b9c5c1b8' 2 'ERROR: -Expected is not 64 hex digits'
} finally {
    Remove-Item -Path $base -Recurse -Force -ErrorAction SilentlyContinue
}
"catalog-precheck: $($n - $fail)/$n PASS"
if ($fail) { exit 1 }
exit 0
