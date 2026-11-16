# Guarded write of the unit A SPI flash with flashrom and a CH341A on the J4004 header (README.md).
# Precondition (operator): the same as spi_read.ps1. Unit A in S5 (plug on, clean shutdown), CH341A jumper on
# 1-2, programmer on USB first, then the 5 wires to J4004 pins 2/3/5/6/7, J4004 pin 1 (VCC) open.
# Use: only for consented writes (the VCN load and repair), each with a written plan.
#
# Guards, in this order. Each guard that fails stops the script before the write:
#   1. The image file has the SHA-256 that the plan names, and the size of the chip.
#   2. The written plan file exists. Its SHA-256 goes into the log.
#   3. The probe finds exactly one chip definition and reports the status register.
#   4. The chip holds the image that the plan names as the base (-BaseSha256). -Restore skips this guard: use it
#      only to write a verified backup back when the chip state is not known.
#   5. The difference touches at most -MaxSectors 4 KiB sectors (not checked with -Restore).
# Then flashrom writes (it erases and writes only the changed blocks and verifies), and two independent reads
# must give the image SHA-256.
# The images hold the UEFI variable store (board identifiers): they stay outside every repository.
# Usage: pwsh -NoProfile -File spi_write.ps1 -Image <file> -ImageSha256 <hex> -BaseSha256 <hex> -PlanFile <file>
#        -OutRoot <dir> [-MaxSectors 16] [-Restore] [-Chip <name>] [-Flashrom <exe>]
param(
    [Parameter(Mandatory)][string]$Image,
    [Parameter(Mandatory)][string]$ImageSha256,
    [string]$BaseSha256 = '',
    [Parameter(Mandatory)][string]$PlanFile,
    [Parameter(Mandatory)][string]$OutRoot,
    [int]$MaxSectors = 16,
    [switch]$Restore,
    [string]$Chip = 'MX25L12835F/MX25L12845E/MX25L12865E',
    [string]$Flashrom = ''
)
$ErrorActionPreference = 'Stop'
$ChipSize = 16777216
$Sector = 4096

function Stop-Write([string]$why) { "SPI-WRITE VERDICT REFUSED: $why (nothing was written)"; exit 2 }

if (-not $Flashrom) {
    $root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
    $Flashrom = Join-Path $root 'scratch\t39\spi-tools\root\ucrt64\bin\flashrom.exe'
}
if (-not $Restore -and -not $BaseSha256) { Stop-Write 'give -BaseSha256 (the image the chip must hold now), or -Restore' }

$utc = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$out = Join-Path $OutRoot "spi-write-unitA-$utc"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'write-log.txt'
function Note([string]$s) { $s; Add-Content $log $s }
$chipArg = @(); if ($Chip) { $chipArg = @('-c', $Chip) }

# Guard 1: the image.
if (-not (Test-Path $Image)) { Stop-Write "no image file $Image" }
$img = [IO.File]::ReadAllBytes((Resolve-Path $Image).Path)
$imgSha = (Get-FileHash $Image -Algorithm SHA256).Hash
if ($imgSha -ne $ImageSha256.ToUpperInvariant()) { Stop-Write "the image SHA-256 is $imgSha, the plan names $ImageSha256" }
if ($img.Length -ne $ChipSize) { Stop-Write "the image has $($img.Length) bytes, the chip has $ChipSize" }

# Guard 2: the plan.
if (-not (Test-Path $PlanFile)) { Stop-Write "no plan file $PlanFile" }
Note "plan $((Resolve-Path $PlanFile).Path) SHA-256 $((Get-FileHash $PlanFile -Algorithm SHA256).Hash)"
Note "image $((Resolve-Path $Image).Path) SHA-256 $imgSha"
Note "mode $(if ($Restore) { 'restore' } else { 'write' }) base $BaseSha256 max-sectors $MaxSectors"

# Guard 3: the probe.
Note '== probe (verbose, no read: records the status register)'
$probe = & $Flashrom -p ch341a_spi @chipArg -V 2>&1 | Out-String
$probe | Set-Content (Join-Path $out 'probe-verbose.txt')
if ($probe -match 'Multiple flash chip definitions match') { Stop-Write 'several chip definitions match: give -Chip <name>' }
if ($probe -notmatch 'Found .* flash chip') { Stop-Write 'no chip found: check the wires, the jumper (1-2) and the USB connection' }
if ($probe -notmatch 'status register is (0x[0-9a-fA-F]{2})') { Stop-Write 'the probe did not report the status register' }
Note "status register before the write: $($Matches[1])"

# Guard 4: the chip holds the base.
$cur = Join-Path $out 'before.bin'
& $Flashrom -p ch341a_spi @chipArg -r $cur 2>&1 | Out-String | Add-Content $log
if (-not (Test-Path $cur)) { Stop-Write 'the read before the write made no file' }
$curSha = (Get-FileHash $cur -Algorithm SHA256).Hash
Note "chip before the write: SHA-256 $curSha"
if (-not $Restore -and $curSha -ne $BaseSha256.ToUpperInvariant()) { Stop-Write "the chip holds $curSha, the plan names the base $BaseSha256" }

# Guard 5: the difference.
$before = [IO.File]::ReadAllBytes($cur)
if ($before.Length -ne $ChipSize) { Stop-Write "the read gave $($before.Length) bytes" }
$ranges = New-Object System.Collections.Generic.List[object]
$sectors = New-Object System.Collections.Generic.HashSet[int]
$diffBytes = 0
$i = 0
while ($i -lt $ChipSize) {
    if ($img[$i] -ne $before[$i]) {
        $start = $i
        while ($i -lt $ChipSize -and $img[$i] -ne $before[$i]) { [void]$sectors.Add([int][Math]::Floor($i / $Sector)); $diffBytes++; $i++ }
        $ranges.Add([ordered]@{ start = ('0x{0:X6}' -f $start); length = ($i - $start) })
    } else { $i++ }
}
[ordered]@{ base = $curSha; image = $imgSha; bytes = $diffBytes; sectors = $sectors.Count; ranges = $ranges } |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'diff.json')
Note "difference: $diffBytes bytes in $($ranges.Count) ranges, $($sectors.Count) sectors of 4 KiB"
if ($diffBytes -eq 0) { Note "SPI-WRITE VERDICT PASS: the chip already holds the image (nothing written); $out"; exit 0 }
if (-not $Restore -and $sectors.Count -gt $MaxSectors) { Stop-Write "the difference touches $($sectors.Count) sectors, more than -MaxSectors $MaxSectors" }

# The write.
Note '== write (flashrom erases and writes the changed blocks, then verifies)'
$w = & $Flashrom -p ch341a_spi @chipArg -w (Resolve-Path $Image).Path 2>&1 | Out-String
$w | Add-Content $log
$flashromOk = $w -match 'VERIFIED'

# Two independent reads.
$hashes = @()
foreach ($n in 1, 2) {
    $f = Join-Path $out "after$n.bin"
    & $Flashrom -p ch341a_spi @chipArg -r $f 2>&1 | Out-String | Add-Content $log
    if (-not (Test-Path $f)) { Note "SPI-WRITE VERDICT FAIL: read $n after the write made no file; restore the backup with -Restore"; exit 1 }
    $h = (Get-FileHash $f -Algorithm SHA256).Hash
    $hashes += $h
    Note "read $n after the write: SHA-256 $h"
}
if ($hashes[0] -ne $imgSha -or $hashes[1] -ne $imgSha) {
    Note "SPI-WRITE VERDICT FAIL: the chip does not hold the image (flashrom verify: $flashromOk); restore the backup with -Restore"
    exit 1
}
Note "SPI-WRITE VERDICT PASS: the chip holds $imgSha ($diffBytes bytes changed, flashrom verify: $flashromOk); $out"
