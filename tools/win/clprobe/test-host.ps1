# The 'clprobe' gate of quick.ps1: builds clprobe.exe and runs the checks that need no OpenCL driver. A loader path
# that does not exist must end in result=no-opencl with exit code 3, and each bad option in usage with exit code 4.
# The GPU runs (pass, and the skip-kernel negative control) are in README.md: they need an OpenCL driver.
#   pwsh -NoProfile -File tools\win\clprobe\test-host.ps1 -Kits <toolchain\nuget> -Out <dir>
param([Parameter(Mandatory)][string]$Kits, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Kits $Kits -Out $Out
if ($LASTEXITCODE) { throw 'clprobe build failed' }
$exe = Join-Path $Out 'clprobe.exe'
$fail = 0
function Expect([string[]]$Arguments, [int]$Code, [string]$Text, [string]$Name) {
    $lines = @(& $exe @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $ok = ($LASTEXITCODE -eq $Code) -and (($lines -join "`n").Contains($Text))
    if ($ok) { "PASS $Name" } else { "FAIL $Name (exit $LASTEXITCODE)"; $lines | Select-Object -Last 3; $script:fail++ }
}
Expect @('--loader', (Join-Path $Out 'absent-OpenCL.dll'), '--bound', '20') 3 'result=no-opencl stage=loader' 'absent loader'
Expect @('--size', '5') 4 'USAGE --size' 'size out of range'
Expect @('--bound', '0') 4 'USAGE --bound' 'bound out of range'
Expect @('--inject', 'other') 4 'USAGE unknown option' 'unknown injection'
Expect @('--device') 4 'USAGE unknown option or missing value' 'missing value'
if ($fail) { throw "$fail clprobe checks failed" }
'PASS: 5 clprobe checks'
