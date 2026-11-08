# Host test of the catalog gate of swap-sys.ps1. Every case stops before the first change: a refusal of the catalog
# check, or (after a match) the existing refusal for a missing -Sha. The restore case stops at the read of the service
# ImagePath of this PC (no bc250kmd service here). No case writes.
#   powershell -NoProfile -File tools\win\kmd-swap\test-swap-sys.ps1
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'swap-sys.ps1'
$base = "HKCU:\Software\bc250-test-swap-sys-$PID"
$a = 'b9c5c1b8d559f5ff16e110bb67508d503a6a6f60f2caf2c0769fbd15f84ac292'
$b = '02c7eed429a46a20825918b23c7adc364f5748f57bacc97b342bfac60d961860'
$fail = 0; $n = 0
function Case([string]$Name, [string[]]$Arguments, [int]$Code, [string[]]$Want, [string[]]$NotWant) {
    $script:n++
    $ErrorActionPreference = 'Continue'   # stderr of the child is output here, not a terminating error (PS 5.1)
    $out = & powershell.exe -NoProfile -NonInteractive -File $script @Arguments 2>&1 | Out-String
    $got = $LASTEXITCODE
    $bad = @($Want | Where-Object { $out -notlike "*$_*" }) + @($NotWant | Where-Object { $_ -and $out -like "*$_*" })
    if ($got -ne $Code -or $bad.Count) { $script:fail++; "FAIL $Name : exit $got (want $Code); lines $($bad -join ' | ')"; $out }
    else { "ok   $Name : exit $got" }
}
try {
    $key = Join-Path $base 'svc'
    New-Item -Path (Join-Path $key 'Parameters\DefaultsV2') -Force | Out-Null
    New-ItemProperty -Path (Join-Path $key 'Parameters\DefaultsV2') -Name CatalogId -Value $a -PropertyType String | Out-Null
    New-ItemProperty -Path (Join-Path $key 'Parameters\DefaultsV2') -Name Schema -Value 2 -PropertyType DWord | Out-Null
    $never = @('kept original', 'image now', 'pnputil', 'UnconfirmedStarts')
    Case 'swap without -Catalog' @('-New', 'x.sys', '-Sha', ('0' * 64), '-ServiceKey', $key) 2 @('-Catalog is required') $never
    Case 'swap of another catalog (A1 s3)' @('-New', 'x.sys', '-Sha', ('0' * 64), '-Catalog', $b, '-ServiceKey', $key) 5 @("DefaultsV2 CatalogId is $a", 'nothing changed') $never
    Case 'match goes on to the next check' @('-New', 'x.sys', '-Catalog', $a, '-ServiceKey', $key) 2 @("CATALOG MATCH $a", '-New and -Sha are required') $never
    Case 'restore does not ask for a catalog' @('-Restore', '-Keep', (Join-Path $env:TEMP "absent-$PID.sys"), '-ServiceKey', $key) 1 @('Cannot find path') @('-Catalog is required', 'CATALOG')
} finally { Remove-Item -Path $base -Recurse -Force -ErrorAction SilentlyContinue }
"swap-sys catalog gate: $($n - $fail)/$n PASS"
if ($fail) { exit 1 }
exit 0
