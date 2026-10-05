# Conformance sweep for the BC-250 H.264 encoder MFT.
#
# Every case encodes a synthetic sequence, decodes it with the inbox H.264 decoder MFT and requires the
# encoder's own GPU reconstruction to be sample-for-sample identical to the decoder's output. That is
# the oracle: a difference means our reconstruction and a conformant decoder disagree, whatever the
# picture looks like. Run from anywhere; it drives the build output in scratch\build\mft-h264.
#
# Usage: pwsh -NoProfile -File tests\sweep.ps1 [-Exe <path>] [-Out <dir>]

[CmdletBinding()]
param(
    [string]$Exe,
    [string]$Out
)

$root = if ($env:BC250_ROOT) { $env:BC250_ROOT }
        else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..')).Path }
if (-not $Exe) { $Exe = Join-Path $root 'scratch\build\mft-h264\mfthost.exe' }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\mft-h264\sweep' }

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "no mfthost.exe at $Exe" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# width height frames gop qp extra-flags
$cases = @(
    @(64, 48, 1, 1, 6, @()),
    @(64, 48, 1, 1, 26, @()),
    @(64, 48, 1, 1, 51, @()),
    @(64, 48, 8, 8, 26, @()),
    @(64, 48, 8, 60, 26, @('--deblock')),
    @(64, 48, 8, 60, 26, @('--deblock', '--still')),
    @(176, 144, 10, 60, 40, @('--deblock')),
    @(320, 240, 6, 3, 18, @('--deblock')),
    @(320, 240, 6, 60, 26, @('--deblock', '--cbr', '--bitrate', '1500000')),
    @(352, 288, 4, 60, 26, @('--deblock', '--gpu-source')),
    @(640, 480, 4, 60, 26, @()),
    @(640, 480, 4, 60, 26, @('--deblock')),
    @(640, 480, 4, 60, 34, @('--deblock')),
    @(640, 480, 6, 60, 26, @('--deblock', '--still')),
    @(640, 482, 3, 60, 26, @('--deblock')),
    @(638, 478, 3, 60, 26, @('--deblock')),
    @(1280, 720, 4, 60, 26, @('--deblock')),
    @(1280, 720, 4, 60, 26, @('--deblock', '--gpu-source')),
    @(1920, 1080, 3, 60, 30, @('--deblock'))
)

# Every quantiser from 16 up, with I and P pictures and deblocking on. This is what guards the three
# hand-entered deblocking tables (8-16, 8-17) row by row: indexA equals the quantiser here, and a wrong
# tc0 row shows up as a reconstruction that a conformant decoder does not reproduce. Below 16 alpha is
# zero and the filter does nothing, so those quantisers only exercise the coefficient path.
foreach ($q in 6..51) {
    $cases += , @(320, 240, 3, 60, $q, @('--deblock'))
}

$pass = 0
$fail = 0
foreach ($c in $cases) {
    $args = @('--encode', '--width', $c[0], '--height', $c[1], '--frames', $c[2],
              '--gop', $c[3], '--qp', $c[4], '--out', $Out) + $c[5]
    $text = & $Exe @args 2>&1 | Out-String
    $ok = $LASTEXITCODE -eq 0
    $exact = ([regex]::Match($text, '(\d+) of (\d+) pictures bit exact')).Groups
    $psnr = ([regex]::Match($text, 'mean PSNR\(Y\) ([0-9.]+) dB')).Groups[1].Value
    $rate = ([regex]::Match($text, '(\d+) bit/s')).Groups[1].Value
    $ms = ([regex]::Match($text, '([0-9.]+) ms per picture')).Groups[1].Value
    $name = "$($c[0])x$($c[1]) f$($c[2]) gop$($c[3]) qp$($c[4]) $($c[5] -join ' ')"
    $verdict = if ($ok) { 'PASS' } else { 'FAIL' }
    if ($ok) { ++$pass } else { ++$fail }
    "{0,-52} {1} exact {2}/{3}  PSNR(Y) {4,6} dB  {5,9} bit/s  {6,6} ms" -f `
        $name, $verdict, $exact[1].Value, $exact[2].Value, $psnr, $rate, $ms
    if (-not $ok) {
        ($text -split "`n" | Select-String -Pattern 'FAIL|differs first') | ForEach-Object { "      $_" }
    }
}
""
"sweep: $pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
