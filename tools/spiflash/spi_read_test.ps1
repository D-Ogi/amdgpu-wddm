# Host test of spi_read.ps1 with the flashrom dummy programmer. No hardware, no device.
# The dummy programmer emulates a 16 MiB chip (W25Q128FV) with a known content: a synthetic image, or a saved
# read when -Image names one. It proves the logic of the read tool with a programmer that is not the CH341A:
# the -Programmer parameter, two equal reads, the verdict line, the hash of the result equal to the source
# image, the refusal of an image that holds one byte value only, and the refusal of a serprog port that is
# absent. It proves nothing about a real programmer or about the wires.
# The work directory is made under BC250_TEST_TMP (a private directory outside repositories) and removed.
# Usage: pwsh -NoProfile -File spi_read_test.ps1 [-Image <saved 16 MiB read>] [-Flashrom <exe>]
param([string]$Image = '', [string]$Flashrom = '')
$ErrorActionPreference = 'Stop'
if (-not $env:BC250_TEST_TMP -or -not (Test-Path -LiteralPath $env:BC250_TEST_TMP -PathType Container)) {
    throw 'set BC250_TEST_TMP to an existing private directory outside repositories'
}
$tool = Join-Path $PSScriptRoot 'spi_read.ps1'
$work = Join-Path $env:BC250_TEST_TMP ('spi-read-test-' + [IO.Path]::GetRandomFileName())
[void][IO.Directory]::CreateDirectory($work)
$common = @(); if ($Flashrom) { $common = @('-Flashrom', $Flashrom) }
$rows = @()
try {
    if (-not $Image) {
        # Synthetic content: a fixed pseudo-random sequence, so no board data is needed.
        $Image = Join-Path $work 'synthetic.bin'
        $data = [byte[]]::new(16MB)
        [Random]::new(250).NextBytes($data)
        [IO.File]::WriteAllBytes($Image, $data)
    }
    $want = (Get-FileHash $Image).Hash
    $chip = Join-Path $work 'chip.bin'
    Copy-Item $Image $chip
    $dummy = "dummy:emulate=W25Q128FV,image=$chip"
    $text = & pwsh -NoProfile -File $tool -OutRoot (Join-Path $work 'out1') -Reads 2 @common -Chip 'W25Q128.V' -Programmer $dummy 2>&1 | Out-String
    $rows += , @(($LASTEXITCODE -eq 0 -and $text -match "SPI-READ VERDICT PASS: 2 equal reads, 16777216 bytes, SHA-256 $want"), 'two reads through the dummy programmer equal the source image')
    $rows += , @(($text -match 'SPI-READ VERDICT PASS.*\(programmer dummy:'), 'the verdict names a programmer that is not the CH341A')
    $dirs = @(Get-ChildItem (Join-Path $work 'out1') -Directory -ErrorAction SilentlyContinue)
    if ($dirs.Count -ne 1) { "no result directory; output of the tool:`n$text" }
    $rows += , @(($dirs.Count -eq 1 -and (Get-Content (Join-Path $dirs[0].FullName 'programmer.txt') -Raw).Trim() -eq $dummy), 'the programmer string is recorded in programmer.txt')
    $rows += , @(((Get-FileHash $chip).Hash -eq $want), 'the emulated chip content is unchanged after the reads')
    $ff = Join-Path $work 'ff.bin'
    $blank = [byte[]]::new(16MB); for ($i = 0; $i -lt $blank.Length; $i++) { $blank[$i] = 0xFF }
    [IO.File]::WriteAllBytes($ff, $blank)
    $text2 = & pwsh -NoProfile -File $tool -OutRoot (Join-Path $work 'out2') -Reads 2 @common -Chip 'W25Q128.V' -Programmer "dummy:emulate=W25Q128FV,image=$ff" 2>&1 | Out-String
    $rows += , @(($LASTEXITCODE -eq 1 -and $text2 -match 'SPI-READ VERDICT FAIL: the image holds one byte value only'), 'an image of one byte value is refused')
    $text3 = & pwsh -NoProfile -File $tool -OutRoot (Join-Path $work 'out3') -Reads 2 @common -Chip 'W25Q128.V' -Programmer 'dummy:emulate=MX25L6436' 2>&1 | Out-String
    $rows += , @(($LASTEXITCODE -ne 0 -and $text3 -notmatch 'VERDICT PASS'), 'a chip that the name does not match gives no PASS')
    # A serprog programmer on a port that does not exist: the DTR step must stop the run before flashrom starts.
    # The stand-in for flashrom leaves a marker file when it is called.
    $marker = Join-Path $work 'flashrom-was-called.txt'
    $standIn = Join-Path $work 'stand-in.cmd'
    [IO.File]::WriteAllText($standIn, "@echo called> `"$marker`"`r`n")
    $text4 = & pwsh -NoProfile -File $tool -OutRoot (Join-Path $work 'out4') -Reads 2 -Flashrom $standIn -Programmer 'serprog:dev=COM250:115200,spispeed=2M' 2>&1 | Out-String
    $rows += , @(($LASTEXITCODE -ne 0 -and $text4 -match 'mode did not set DTR on for COM250' -and $text4 -notmatch 'VERDICT PASS' -and -not (Test-Path $marker)), 'a serprog programmer on an absent port is refused before flashrom starts')
}
finally { [IO.Directory]::Delete($work, $true) }
$bad = 0
foreach ($r in $rows) { "$(if ($r[0]) { 'PASS' } else { $bad++; 'FAIL' }) $($r[1])" }
"RESULT: $(if ($bad) { 'FAIL' } else { 'ALL PASS' }) ($($rows.Count) cases)"
exit $(if ($bad) { 1 } else { 0 })
