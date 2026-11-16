# Backup of the unit A SPI flash with flashrom and a CH341A on the J4004 header (README.md).
# Precondition (operator): unit A in S5 (plug on, clean shutdown), CH341A jumper on 1-2, programmer on USB
# first, then the 5 wires CS/MISO/GND/CLK/MOSI to J4004 pins 2/3/5/6/7, J4004 pin 1 (VCC) open.
# CAUTION: flashrom 1.2 calls the chip unlock hook for -r too (prepare_flash_access). When block-protect bits
# are set, a read clears them with WREN+WRSR. The script records the status register with a probe-only -V
# run before the first read, so the original value is never lost.
# The image holds the UEFI variable store (board identifiers): it stays outside every repository.
# Usage: pwsh -NoProfile -File spi_read.ps1 -OutRoot <dir> [-NasRoot <dir>] [-Chip <name>] [-Reads 3] [-Flashrom <exe>]
param(
    [Parameter(Mandatory)][string]$OutRoot,
    [string]$NasRoot = '',
    [string]$Chip = 'MX25L12835F/MX25L12845E/MX25L12865E',
    [int]$Reads = 3,
    [string]$Flashrom = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Flashrom) {
    $root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
    $Flashrom = Join-Path $root 'scratch\t39\spi-tools\root\ucrt64\bin\flashrom.exe'
}
$utc = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$name = "spi-backup-unitA-$utc"
$out = Join-Path $OutRoot $name
New-Item -ItemType Directory -Force $out | Out-Null
$chipArg = @(); if ($Chip) { $chipArg = @('-c', $Chip) }

"== probe (verbose, no read: records the status register before any unlock)"
$probe = & $Flashrom -p ch341a_spi @chipArg -V 2>&1 | Out-String
$probe | Set-Content (Join-Path $out 'probe-verbose.txt')
$probe -split "`n" | Select-String 'Found|status register is'
if ($probe -match 'Multiple flash chip definitions match') { throw 'several chip definitions match: run again with -Chip <name>' }
if ($probe -notmatch 'Found .* flash chip') { throw 'no chip found: check the wires, the jumper (1-2) and the USB connection' }
if ($probe -notmatch 'status register is 0x[0-9a-fA-F]{2}') { throw 'the probe did not report the status register' }

$hashes = @()
for ($i = 1; $i -le $Reads; $i++) {
    $f = Join-Path $out "read$i.bin"
    "== read $i"
    & $Flashrom -p ch341a_spi @chipArg -r $f 2>&1 | Out-String | Add-Content (Join-Path $out 'reads.txt')
    if (-not (Test-Path $f)) { throw "read $i made no file" }
    $h = (Get-FileHash $f).Hash
    $hashes += $h
    "read $i $((Get-Item $f).Length) bytes $h"
}
$distinct = @($hashes | Select-Object -Unique)
if ($distinct.Count -ne 1) { "SPI-READ VERDICT FAIL: the reads differ ($($distinct.Count) distinct hashes); check the wires and run again"; exit 1 }
$bytes = [IO.File]::ReadAllBytes((Join-Path $out 'read1.bin'))
if ([Linq.Enumerable]::Count([Linq.Enumerable]::Distinct([byte[]]$bytes)) -le 1) { "SPI-READ VERDICT FAIL: the image holds one byte value only (no contact or no power on the chip)"; exit 1 }
"$($hashes[0])  read1.bin" | Set-Content (Join-Path $out 'SHA256SUMS')
if ($NasRoot) {
    $nas = Join-Path $NasRoot $name
    New-Item -ItemType Directory -Force $nas | Out-Null
    Copy-Item (Join-Path $out 'read1.bin'), (Join-Path $out 'SHA256SUMS'), (Join-Path $out 'probe-verbose.txt') $nas
    if ((Get-FileHash (Join-Path $nas 'read1.bin')).Hash -ne $hashes[0]) { "SPI-READ VERDICT FAIL: the NAS copy differs"; exit 1 }
}
"SPI-READ VERDICT PASS: $Reads equal reads, $($bytes.Length) bytes, SHA-256 $($hashes[0]); $out"
