# Checks the tracked shader headers against the compiler and the record that made them. No device, no lab
# and no link step, so it belongs in the fast gate.
#
#   pwsh -NoProfile -File tools\win\conformance-clients\check-shaders.ps1 -Kits <BC250_ROOT>\toolchain\nuget
#
# What it proves:
#   1. gen\dxc.txt carries the workspace root as <BC250_ROOT> and not as an absolute path of one machine.
#   2. The dxc of -Kits is the compiler the record names, by SHA-256.
#   3. Replaying the recorded command lines reproduces every tracked header in gen\*.h, line for line.
# Line comparison, not a byte compare: the headers are checked out with LF per .gitattributes, and dxc
# writes CRLF.
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$here = $PSScriptRoot
$gen = Join-Path $here 'gen'
$record = Join-Path $gen 'dxc.txt'
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { (Resolve-Path (Join-Path $Kits '..\..')).Path }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\conformance-shaders-check' }
$dxc = Join-Path $Kits "microsoft.windows.sdk.cpp\c\bin\$KitVersion\x64\dxc.exe"
if (-not (Test-Path -LiteralPath $dxc)) { throw "dxc not found: $dxc" }

$lines = @(Get-Content -LiteralPath $record)
if ($lines.Count -lt 4) { throw "$record holds $($lines.Count) lines; expected the path, the version, the hash and one line per program" }
if ($lines[0] -notmatch '^dxc <BC250_ROOT>\\') {
    throw "gen\dxc.txt line 1 is '$($lines[0])'; the dxc path must start with the placeholder <BC250_ROOT>, never with a drive letter"
}
if ($lines[2] -notmatch '^dxc sha256 ([0-9A-Fa-f]{64})$') { throw "gen\dxc.txt line 3 is not a dxc sha256 line: '$($lines[2])'" }
$recorded = $Matches[1]
$actual = (Get-FileHash -LiteralPath $dxc).Hash
if ($actual -ne $recorded) {
    throw "dxc of -Kits has sha256 $actual, and gen\dxc.txt records $recorded. Rebuild the headers with tools\win\conformance-clients\build.ps1 and commit the result, or point -Kits at the recorded SDK"
}

New-Item -ItemType Directory -Force $Out | Out-Null
$programs = 0
$failures = @()
foreach ($line in $lines[3..($lines.Count - 1)]) {
    $argv = @($line -split ' +' | Where-Object { $_ })
    if ($argv.Count -lt 3 -or $argv[0] -ne 'dxc') { throw "gen\dxc.txt holds a line that is not a dxc command: '$line'" }
    $argv = $argv[1..($argv.Count - 1)]
    $headerArg = [Array]::IndexOf($argv, '-Fh')
    if ($headerArg -lt 0 -or $headerArg + 1 -ge $argv.Count) { throw "the recorded command has no -Fh header: '$line'" }
    $relative = $argv[$headerArg + 1]
    $tracked = Join-Path $here $relative
    if (-not (Test-Path -LiteralPath $tracked)) { throw "the record names $relative, which is not in the repository" }
    $fresh = Join-Path $Out (Split-Path -Leaf $relative)
    $argv[$headerArg + 1] = $fresh
    $argv[$argv.Count - 1] = Join-Path $here $argv[$argv.Count - 1]
    & $dxc @argv
    if ($LASTEXITCODE -ne 0) { throw "dxc failed for $relative ($LASTEXITCODE)" }
    $difference = Compare-Object @(Get-Content -LiteralPath $tracked) @(Get-Content -LiteralPath $fresh) -SyncWindow 0
    if ($difference) {
        $failures += "$relative differs from the rebuild in $($difference.Count) line(s)"
        Write-Host "FAIL $relative"
    } else {
        Write-Host "ok   $relative"
    }
    $programs++
}
$headers = @(Get-ChildItem -LiteralPath $gen -Filter '*.h' -File)
if ($headers.Count -ne $programs) {
    $failures += "gen holds $($headers.Count) headers and the record covers $programs; every header must come from a recorded command"
}
if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAIL $_" }
    throw "conformance shader headers do not match their record ($($failures.Count) failure(s))"
}
Write-Host "  $programs of $programs tracked headers reproduce from gen\dxc.txt"
