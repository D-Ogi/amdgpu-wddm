# The 'bd167probe' gate of quick.ps1: builds bd167probe.exe twice (two output folders, the same SHA-256) and runs the
# checks that need no BC-250: the verdict rules on made-up runs (--self-test), and on a PC without the BC-250 GPU a
# real run that ends in no-adapter (exit 3) without sending the escape to any other adapter. The lab arm is in
# README.md.
#   pwsh -NoProfile -File tools\win\bd167probe\test-host.ps1 -Kits <toolchain\nuget> -Out <dir>
param([Parameter(Mandatory)][string]$Kits, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
foreach ($d in 'a', 'b') {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Kits $Kits -Out (Join-Path $Out $d) | Out-Host
    if ($LASTEXITCODE) { throw "bd167probe build $d failed" }
}
$exe = Join-Path $Out 'a\bd167probe.exe'
$fail = 0
$ha = (Get-FileHash -LiteralPath $exe).Hash
$hb = (Get-FileHash -LiteralPath (Join-Path $Out 'b\bd167probe.exe')).Hash
if ($ha -eq $hb) { "PASS the same SHA-256 from two output folders ($($ha.Substring(0, 8)))" } else { "FAIL two builds differ: $ha $hb"; $fail++ }
function Expect([string[]]$Arguments, [int]$Code, [string]$Text, [string]$Name) {
    $lines = @(& $exe @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $ok = ($LASTEXITCODE -eq $Code) -and (($lines -join "`n").Contains($Text))
    if ($ok) { "PASS $Name" } else { "FAIL $Name (exit $LASTEXITCODE)"; $lines | Select-Object -Last 3; $script:fail++ }
}
Expect @('--self-test') 0 'PASS: 10 self-test checks' 'verdict rules'
Expect @() 4 'USAGE --expect is required' 'no expectation'
Expect @('--expect', 'other') 4 'USAGE --expect is release or lab' 'unknown expectation'
Expect @('--expect', 'lab', '--pattern', 'xyz') 4 'USAGE --pattern is a hex number' 'bad pattern'
Expect @('--expect') 4 'USAGE unknown option or missing value' 'missing value'
# A development PC has no 1002:13FE adapter: the run must stop before any escape. On a BC-250 the check is skipped.
$bc250 = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'PCI\VEN_1002&DEV_13FE*' }).Count
if ($bc250) { 'SKIP no-adapter: this PC has the BC-250 GPU' }
else { Expect @('--expect', 'release', '--no-witness') 3 'result=no-adapter' 'no BC-250 adapter' }
if ($fail) { throw "$fail bd167probe checks failed" }
'PASS: bd167probe host checks'
