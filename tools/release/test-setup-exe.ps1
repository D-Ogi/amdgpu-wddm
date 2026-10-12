# Tests the single-file installer end to end, with nothing installed and nothing outside -Out written (owner,
# 2026-10-12: "dlaczego setup.exe nie zawiera zipa w sobie? to lepsza metoda!").
#
#   pwsh -File tools\release\test-setup-exe.ps1 -Setup <amdgpu_wddm_setup.exe> -Out <work dir>
#
# -Setup is the built setup window (tools\win\amdgpu_wddm_setup\build.ps1 writes it). The test makes a small package
# of its own - a manifest, an installer and a few files, the layout Payload.cs requires - packs it with
# make-setup-exe.ps1, and then drives the reader through the exe's own --smoke-payload mode, which reads and checks
# the payload exactly as an install reads it and unpacks it into a plain folder here instead of
# %ProgramData%\amdgpu-wddm\setup (so this needs no administrator rights; the permission list of the real folder is
# checked by the window's unit tests, Payload.Access).
#
# The positive case: the exe carries the package, every file comes back byte for byte, and the summary names the
# release. The negative controls, each of which MUST be refused:
#
#   1. one byte of the payload changed             - the payload hash of the trailer no longer matches
#   2. one byte of the file list changed           - the list hash no longer matches
#   3. a file of the zip replaced by other bytes   - the file's own SHA-256 no longer matches the list
#   4. a file of the zip removed                   - the list names a file the payload does not hold
#   5. the magic of the trailer changed            - not a package: the exe behaves as a setup.exe without one
#   6. the payload length of the trailer enlarged  - the section does not fit the file
#   7. the plain setup window, with nothing appended - no package, which is the ordinary setup.exe beside files\
#
# Exit 0 = every case behaved. A failure prints FAIL and what was expected.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Setup,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'

$setupExe = (Resolve-Path -LiteralPath $Setup).Path
$root = (New-Item -ItemType Directory -Force $Out).FullName
$failed = 0
$passed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { $script:passed++ } else { $script:failed++; Write-Host "FAIL $What" -ForegroundColor Red }
}

# 1. A small release of our own, in the layout of a real one: setup.exe at the top, the package in files\ beside it.
$top = Join-Path $root 'package'
Remove-Item -LiteralPath $top -Recurse -Force -ErrorAction SilentlyContinue
$files = Join-Path $top 'files'
New-Item -ItemType Directory -Force (Join-Path $files 'installer'), (Join-Path $files 'driver') | Out-Null
Copy-Item -LiteralPath $setupExe -Destination (Join-Path $top 'setup.exe')
$version = '0.0.0-test-setup-exe'
@"
{ "version": "$version", "files": [] }
"@ | Set-Content -LiteralPath (Join-Path $files 'manifest.json') -Encoding utf8
'# the installer of the test package: it is never started here' |
    Set-Content -LiteralPath (Join-Path $files 'installer\install.ps1') -Encoding utf8
'the driver of the test package' | Set-Content -LiteralPath (Join-Path $files 'driver\amdgpu_wddm.sys') -Encoding utf8
# A file with bytes that are not text, so the payload is not only compressible text.
[IO.File]::WriteAllBytes((Join-Path $files 'driver\amdgpu_wddm.cat'), (1..4096 | ForEach-Object { [byte](($_ * 37) % 256) }))
$packageFiles = @(Get-ChildItem -LiteralPath $top -Recurse -File)
Write-Host "test package: $($packageFiles.Count) files, $version"

# 2. Pack it. make-setup-exe.ps1 -Check reads the exe back and compares every file with the one on the disk, so the
#    packer's own gate runs here too.
$packed = Join-Path $root 'amdgpu-wddm-setup-test.exe'
$log = & (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $top -Out $packed -Check 2>&1 | ForEach-Object { [string]$_ }
$log | ForEach-Object { Write-Host "  $_" }
# make-setup-exe.ps1 stops on any error, so reaching this line at all is the packer finishing.
Check (@($log | Where-Object { $_ -match 'program \d+ \+ package \d+ \+ list \d+ \+ trailer 120' }).Count -eq 1) `
    'the packer states the size of each section'
Check (Test-Path -LiteralPath $packed -PathType Leaf) 'the single-file installer was written'
Check (@($log | Where-Object { $_ -match 'byte for byte' }).Count -eq 1) 'the packer states that the payload holds the files of the release'
$packedBytes = [IO.File]::ReadAllBytes($packed)
$plainBytes = [IO.File]::ReadAllBytes($setupExe)
Check ($packedBytes.Length -gt $plainBytes.Length) 'the single file is larger than the window alone'
$front = New-Object byte[] $plainBytes.Length
[Array]::Copy($packedBytes, 0, $front, 0, $plainBytes.Length)
$sha = [Security.Cryptography.SHA256]::Create()
try { $frontSame = (($sha.ComputeHash($front) | ForEach-Object { $_.ToString('x2') }) -join '') -eq
    (($sha.ComputeHash($plainBytes) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $sha.Dispose() }
Check $frontSame 'the program image at the front of the single file is the setup window, byte for byte'

# 3. The reader, through the exe itself.
function Invoke-Smoke([string]$Exe, [string]$Name) {
    $dir = Join-Path $root "smoke-$Name"
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    $out = & $Exe --smoke-payload $dir 2>&1 | ForEach-Object { [string]$_ }
    $code = $LASTEXITCODE
    $report = Join-Path $dir 'payload.txt'
    $text = ''
    if (Test-Path -LiteralPath $report -PathType Leaf) { $text = Get-Content -LiteralPath $report -Raw }
    [pscustomobject]@{ code = $code; text = ($out -join "`n"); dir = $dir; report = $text }
}

$ok = Invoke-Smoke $packed 'ok'
Check ($ok.code -eq 0) "the packed installer reads its own payload (exit $($ok.code): $($ok.text))"
Check ($ok.report -match 'payload ok') 'the summary says the payload is good'
Check ($ok.report -match [regex]::Escape($version)) 'the summary names the release of the payload'
Check ($ok.report -match "files $($packageFiles.Count)") "the summary counts the $($packageFiles.Count) files"
$unpacked = Join-Path $ok.dir "unpacked\$version"
Check (Test-Path -LiteralPath (Join-Path $unpacked 'files\manifest.json') -PathType Leaf) 'the package was unpacked'
$same = $true
foreach ($f in $packageFiles) {
    $rel = $f.FullName.Substring($top.Length + 1)
    $there = Join-Path $unpacked $rel
    if (-not (Test-Path -LiteralPath $there -PathType Leaf)) { $same = $false; Write-Host "  missing: $rel"; continue }
    if ((Get-FileHash -LiteralPath $there -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash) {
        $same = $false; Write-Host "  different: $rel"
    }
}
Check $same 'every unpacked file is the file of the release, byte for byte'

# 4. The negative controls. Each one damages a copy of the single file and the reader must refuse it; a reader that
#    unpacked a doctored payload would be the whole point of the manifest lost.
$trailerSize = 120
function Test-Damaged([string]$Name, [scriptblock]$Damage, [int[]]$Expect, [string]$Expected) {
    $exe = Join-Path $root "damaged-$Name.exe"
    $bytes = [IO.File]::ReadAllBytes($packed)
    & $Damage $bytes
    [IO.File]::WriteAllBytes($exe, $bytes)
    $r = Invoke-Smoke $exe $Name
    Check ($Expect -contains $r.code) "$Name is refused ($Expected; exit $($r.code): $($r.text))"
}
# The trailer's own layout, read here so the damage lands where the format says it does.
$tOff = $packedBytes.Length - $trailerSize
$payloadOffset = [BitConverter]::ToInt64($packedBytes, $tOff + 24)
$payloadLength = [BitConverter]::ToInt64($packedBytes, $tOff + 32)
$listOffset = [BitConverter]::ToInt64($packedBytes, $tOff + 40)
Check ($payloadOffset -eq $plainBytes.Length) 'the payload starts where the program image ends'
Check ($payloadLength -gt 0 -and ($payloadOffset + $payloadLength) -le $listOffset) 'the payload fits before the file list'

Test-Damaged 'payload-byte' { param($b) $b[$payloadOffset + 64] = $b[$payloadOffset + 64] -bxor 0xFF } @(4) 'the payload hash no longer matches'
Test-Damaged 'list-byte' { param($b) $b[$listOffset + 10] = $b[$listOffset + 10] -bxor 0xFF } @(4) 'the file list hash no longer matches'
Test-Damaged 'magic' { param($b) $b[$b.Length - $trailerSize] = 0x58 } @(3) 'not a package at all'
Test-Damaged 'length' { param($b)
    $bad = [BitConverter]::GetBytes([int64]($payloadLength + 4096))
    [Array]::Copy($bad, 0, $b, $b.Length - $trailerSize + 32, 8) } @(3, 4) 'the section does not fit the file'

# A file of the zip replaced, and a file removed: the payload is rebuilt, so its own hash is right and only the file
# list can catch it. This is the case a plain hash of the whole payload would miss.
function New-RepackedExe([string]$Name, [scriptblock]$Change) {
    $stage = Join-Path $root "repack-$Name"
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $top -Destination $stage -Recurse
    & $Change $stage
    $exe = Join-Path $root "repacked-$Name.exe"
    # Pack the changed folder, then put the file list and trailer of the GOOD package back: the payload is now
    # self-consistent but no longer the one the list describes.
    & (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $stage -Out $exe | Out-Null
    $changed = [IO.File]::ReadAllBytes($exe)
    $cOff = $changed.Length - $trailerSize
    $cPayloadOffset = [BitConverter]::ToInt64($changed, $cOff + 24)
    $cPayloadLength = [BitConverter]::ToInt64($changed, $cOff + 32)
    $out = New-Object IO.MemoryStream
    $out.Write($changed, 0, [int]($cPayloadOffset + $cPayloadLength))
    # The good list and trailer, with the offsets of this payload.
    $good = [IO.File]::ReadAllBytes($packed)
    $gOff = $good.Length - $trailerSize
    $gListOffset = [BitConverter]::ToInt64($good, $gOff + 40)
    $gListLength = [BitConverter]::ToInt64($good, $gOff + 48)
    $out.Write($good, [int]$gListOffset, [int]$gListLength)
    $trailer = New-Object byte[] $trailerSize
    [Array]::Copy($good, $gOff, $trailer, 0, $trailerSize)
    [Array]::Copy([BitConverter]::GetBytes([int64]$cPayloadOffset), 0, $trailer, 24, 8)
    [Array]::Copy([BitConverter]::GetBytes([int64]$cPayloadLength), 0, $trailer, 32, 8)
    [Array]::Copy([BitConverter]::GetBytes([int64]($cPayloadOffset + $cPayloadLength)), 0, $trailer, 40, 8)
    # The payload hash of this payload, so only the file list is left to refuse it.
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $section = New-Object byte[] $cPayloadLength
        [Array]::Copy($changed, [int]$cPayloadOffset, $section, 0, [int]$cPayloadLength)
        [Array]::Copy($sha.ComputeHash($section), 0, $trailer, 56, 32)
    } finally { $sha.Dispose() }
    $out.Write($trailer, 0, $trailerSize)
    [IO.File]::WriteAllBytes($exe, $out.ToArray())
    $out.Dispose()
    $exe
}
$swapped = New-RepackedExe 'file-swapped' { param($stage)
    'a driver this release never built' | Set-Content -LiteralPath (Join-Path $stage 'files\driver\amdgpu_wddm.sys') -Encoding utf8 }
$r = Invoke-Smoke $swapped 'file-swapped'
Check ($r.code -eq 4) "a file of the payload replaced is refused (exit $($r.code): $($r.text))"
$removed = New-RepackedExe 'file-removed' { param($stage)
    Remove-Item -LiteralPath (Join-Path $stage 'files\driver\amdgpu_wddm.cat') -Force }
$r = Invoke-Smoke $removed 'file-removed'
Check ($r.code -eq 4) "a file of the payload removed is refused (exit $($r.code): $($r.text))"

# 7. The plain window, with nothing appended: no package, which is what the setup.exe of an unpacked release is.
$r = Invoke-Smoke $setupExe 'plain'
Check ($r.code -eq 3) "the window without a payload says so (exit $($r.code): $($r.text))"
Check ($r.report -match 'no payload') 'the summary of a window without a payload says there is none'

if ($failed) { Write-Host "$passed checks passed, $failed FAILED" -ForegroundColor Red; exit 1 }
# The summary goes to the output stream, not to the host: tools\win\amdgpu_wddm_setup\build.ps1 runs this test as
# its last gate and reads this exact line.
"single-file installer: $passed checks passed, 0 failed"
"  the payload was read, checked and unpacked, and 7 damaged or empty exes were refused"
