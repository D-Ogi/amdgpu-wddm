# Builds the single-file installer: ONE exe that carries the whole release inside it (owner, 2026-10-12:
# "dlaczego setup.exe nie zawiera zipa w sobie? to lepsza metoda!").
#
#   pwsh -File tools\release\make-setup-exe.ps1 -PackageTop <unpacked release folder> -Out <file.exe> [-Check]
#
# -PackageTop is the folder the release zip unpacks to: setup.exe in it and the whole package in files\ beside it
# (the layout of 0.7.216.100-tester.28 and later). The output is
#
#   [ setup.exe, byte for byte ][ the package as a zip ][ the file list ][ a 120-byte trailer ]
#
# The sections stand after the end of the program image, so Windows loads the exe exactly as it loads setup.exe and
# the appended bytes are never executed. The trailer says where each section is and what it hashes to; the file list
# names every file with its SHA-256 and size. tools\win\amdgpu_wddm_setup\src\Payload.cs reads all three, unpacks the
# package to %ProgramData%\amdgpu-wddm\setup\<version>\ (administrators and SYSTEM only), checks every file against
# the list before anything uses it, and removes the folder when the run is over.
#
# -Check reads the exe back: the trailer, both hashes, and then every file of the payload against the file on the
# disk, byte for byte. -ReleaseZip <release zip> adds the same comparison against the entries of the published archive, so
# that the one file and the zip of a release are proven to hold the same files. The release recipe (build-release.ps1) always runs it, so a package whose single exe does not
# hold the files of its zip cannot be published.
#
# Nothing here signs anything: the appended sections would break an Authenticode signature of the exe, and the
# release signs the driver package and its catalog, not setup.exe (tools\release\README.md).
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageTop,
    [Parameter(Mandatory)][string]$Out,
    [switch]$Check,
    [string]$ReleaseZip
)
$ErrorActionPreference = 'Stop'

$magic = [Text.Encoding]::ASCII.GetBytes('AMDGPU-WDDM-PKG1')   # 16 bytes
$format = 1
$trailerSize = 120
$utf8 = New-Object Text.UTF8Encoding $false

function Get-Sha256Hex([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant() }
function Get-BytesSha256Hex([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { (($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant() } finally { $sha.Dispose() }
}

$top = (Resolve-Path -LiteralPath $PackageTop).Path.TrimEnd('\')
$setup = Join-Path $top 'setup.exe'
$files = Join-Path $top 'files'
foreach ($p in $setup, (Join-Path $files 'manifest.json')) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "not an unpacked release: no $p" } }
$manifest = Get-Content -LiteralPath (Join-Path $files 'manifest.json') -Raw | ConvertFrom-Json
$version = [string]$manifest.version
if (-not $version) { throw "manifest.json in $files names no version" }

# Every file of the unpacked release, with the path the payload keeps: setup.exe at the top and files\... under it.
# The list is sorted, so the same release gives the same bytes.
$entries = @(Get-ChildItem -LiteralPath $top -Recurse -File | ForEach-Object {
    [pscustomobject]@{ path = $_.FullName.Substring($top.Length + 1); full = $_.FullName }
} | Sort-Object { $_.path.ToLowerInvariant() })
if (-not @($entries | Where-Object { $_.path -ieq 'setup.exe' }).Count) { throw "no setup.exe in $top" }
Write-Host "single-file setup: $($entries.Count) files of $version from $top"

# 1. The package as a zip, written from the sorted list so that the archive is the same for the same release.
$work = Join-Path (Split-Path $Out) 'make-setup-exe.tmp'
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
[void][IO.Directory]::CreateDirectory($work)
$zipPath = Join-Path $work 'payload.zip'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipStream = [IO.File]::Open($zipPath, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite)
try {
    $zip = New-Object IO.Compression.ZipArchive($zipStream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($e in $entries) {
            $entry = $zip.CreateEntry(($e.path -replace '\\', '/'), [IO.Compression.CompressionLevel]::Optimal)
            # One fixed time stamp: the payload of one release is the same bytes wherever it is built.
            $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
            $to = $entry.Open()
            try { $from = [IO.File]::OpenRead($e.full); try { $from.CopyTo($to) } finally { $from.Dispose() } } finally { $to.Dispose() }
        }
    } finally { $zip.Dispose() }
} finally { $zipStream.Dispose() }

# 2. The file list, as Payload.ParseList reads it.
$lines = New-Object Collections.Generic.List[string]
$lines.Add("version`t$version")
foreach ($e in $entries) {
    $info = Get-Item -LiteralPath $e.full
    $lines.Add("file`t$(Get-Sha256Hex $e.full)`t$($info.Length)`t$($e.path)")
}
$listBytes = $utf8.GetBytes((($lines -join "`n") + "`n"))

# 3. setup.exe, the two sections and the trailer, in one file.
$setupBytes = [IO.File]::ReadAllBytes($setup)
$payloadBytes = [IO.File]::ReadAllBytes($zipPath)
$payloadOffset = [int64]$setupBytes.Length
$listOffset = $payloadOffset + $payloadBytes.Length
$trailer = New-Object byte[] $trailerSize
[Array]::Copy($magic, 0, $trailer, 0, $magic.Length)
[Array]::Copy([BitConverter]::GetBytes([int32]$format), 0, $trailer, 16, 4)
[Array]::Copy([BitConverter]::GetBytes([int32]0), 0, $trailer, 20, 4)
[Array]::Copy([BitConverter]::GetBytes([int64]$payloadOffset), 0, $trailer, 24, 8)
[Array]::Copy([BitConverter]::GetBytes([int64]$payloadBytes.Length), 0, $trailer, 32, 8)
[Array]::Copy([BitConverter]::GetBytes([int64]$listOffset), 0, $trailer, 40, 8)
[Array]::Copy([BitConverter]::GetBytes([int64]$listBytes.Length), 0, $trailer, 48, 8)
function Copy-HexInto([byte[]]$Target, [int]$At, [string]$Hex) {
    for ($i = 0; $i -lt 32; $i++) { $Target[$At + $i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16) }
}
Copy-HexInto $trailer 56 (Get-BytesSha256Hex $payloadBytes)
Copy-HexInto $trailer 88 (Get-BytesSha256Hex $listBytes)
$outStream = [IO.File]::Open($Out, [IO.FileMode]::Create, [IO.FileAccess]::Write)
try {
    $outStream.Write($setupBytes, 0, $setupBytes.Length)
    $outStream.Write($payloadBytes, 0, $payloadBytes.Length)
    $outStream.Write($listBytes, 0, $listBytes.Length)
    $outStream.Write($trailer, 0, $trailer.Length)
} finally { $outStream.Dispose() }
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$outHash = Get-Sha256Hex $Out
'  {0}: {1} bytes (program {2} + package {3} + list {4} + trailer {5}), SHA256 {6}' -f
    (Split-Path $Out -Leaf), (Get-Item -LiteralPath $Out).Length, $setupBytes.Length, $payloadBytes.Length, $listBytes.Length, $trailerSize, $outHash

# 4. The gate: read the exe back as Payload.cs does, and compare every file with the one on the disk.
if ($Check) {
    $stream = [IO.File]::Open($Out, [IO.FileMode]::Open, [IO.FileAccess]::Read)
    try {
        $t = New-Object byte[] $trailerSize
        $stream.Seek(-$trailerSize, [IO.SeekOrigin]::End) | Out-Null
        if ($stream.Read($t, 0, $trailerSize) -ne $trailerSize) { throw 'the trailer could not be read back' }
        for ($i = 0; $i -lt $magic.Length; $i++) { if ($t[$i] -ne $magic[$i]) { throw 'the trailer has no magic' } }
        if ([BitConverter]::ToInt32($t, 16) -ne $format) { throw 'the trailer names another format' }
        $po = [BitConverter]::ToInt64($t, 24); $pl = [BitConverter]::ToInt64($t, 32)
        $lo = [BitConverter]::ToInt64($t, 40); $ll = [BitConverter]::ToInt64($t, 48)
        if ($po -ne $payloadOffset -or $pl -ne $payloadBytes.Length -or $lo -ne $listOffset -or $ll -ne $listBytes.Length) { throw 'the trailer names other sections' }
        function Read-Section([IO.Stream]$S, [int64]$Offset, [int64]$Length) {
            $S.Seek($Offset, [IO.SeekOrigin]::Begin) | Out-Null
            $b = New-Object byte[] $Length
            $got = 0
            while ($got -lt $Length) { $n = $S.Read($b, $got, $Length - $got); if ($n -le 0) { break }; $got += $n }
            if ($got -ne $Length) { throw 'a section ends early' }
            return $b
        }
        $backPayload = Read-Section $stream $po $pl
        $backList = Read-Section $stream $lo $ll
        if ((Get-BytesSha256Hex $backPayload) -ne (Get-BytesSha256Hex $payloadBytes)) { throw 'the package section does not hash to the trailer' }
        if ((Get-BytesSha256Hex $backList) -ne (Get-BytesSha256Hex $listBytes)) { throw 'the file list section does not hash to the trailer' }
        # Every file of the payload against the file of the unpacked release, byte for byte.
        $listText = $utf8.GetString($backList)
        $listed = @{}
        foreach ($line in ($listText -split "`n")) {
            if (-not $line.Trim()) { continue }
            $p = $line -split "`t"
            if ($p[0] -eq 'version') { if ($p[1] -ne $version) { throw "the file list names version $($p[1])" }; continue }
            if ($p[0] -ne 'file' -or $p.Count -ne 4) { throw "the file list has an unknown line: $line" }
            $listed[$p[3]] = @{ sha = $p[1]; size = [int64]$p[2] }
        }
        if ($listed.Count -ne $entries.Count) { throw "the file list has $($listed.Count) files, the release has $($entries.Count)" }
        $payloadStream = New-Object IO.MemoryStream (,$backPayload)
        try {
            $archive = New-Object IO.Compression.ZipArchive($payloadStream, [IO.Compression.ZipArchiveMode]::Read)
            try {
                $inZip = @($archive.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
                if ($inZip.Count -ne $entries.Count) { throw "the package holds $($inZip.Count) files, the release has $($entries.Count)" }
                foreach ($entry in $inZip) {
                    $rel = $entry.FullName -replace '/', '\'
                    if (-not $listed.ContainsKey($rel)) { throw "the package holds $rel, which the file list does not name" }
                    $disk = Join-Path $top $rel
                    if (-not (Test-Path -LiteralPath $disk -PathType Leaf)) { throw "the package holds $rel, which the release does not have" }
                    $ms = New-Object IO.MemoryStream
                    $es = $entry.Open()
                    try { $es.CopyTo($ms) } finally { $es.Dispose() }
                    $bytes = $ms.ToArray(); $ms.Dispose()
                    $want = $listed[$rel]
                    if ($bytes.Length -ne $want.size) { throw "$rel is $($bytes.Length) bytes in the package and $($want.size) in the file list" }
                    $hex = Get-BytesSha256Hex $bytes
                    if ($hex -ne $want.sha.ToUpperInvariant()) { throw "$rel does not match the hash the file list gives" }
                    if ($hex -ne (Get-Sha256Hex $disk)) { throw "$rel in the package is not the file of the release" }
                }
            } finally { $archive.Dispose() }
        } finally { $payloadStream.Dispose() }
        '  gate: the payload holds the {0} files of {1}, each byte for byte the file of the release' -f $entries.Count, $version
        # -ReleaseZip: the release zip of the same package, entry by entry against the file list of the payload. The loop
        # above compares the payload with the folder the zip was written from, which is the same content; this
        # compares it with the published archive itself, which is what the owner asked to be proven (2026-10-12,
        # requirement C of the single file: the payload holds the files of the zip). Compress-Archive writes the
        # release folder itself, so every entry starts with that folder's name and the first segment comes off.
        if ($ReleaseZip) {
            $relZipPath = (Resolve-Path -LiteralPath $ReleaseZip).Path
            $zf = [IO.Compression.ZipFile]::OpenRead($relZipPath)
            try {
                $seen = @{}
                foreach ($entry in @($zf.Entries | Where-Object { -not $_.FullName.EndsWith('/') })) {
                    $rel = ($entry.FullName -replace '/', '\')
                    $cut = $rel.IndexOf('\')
                    if ($cut -lt 0) { throw "the zip holds $rel outside the release folder" }
                    $rel = $rel.Substring($cut + 1)
                    if (-not $listed.ContainsKey($rel)) { throw "the zip holds $rel, which the payload does not" }
                    $ms = New-Object IO.MemoryStream
                    $es = $entry.Open()
                    try { $es.CopyTo($ms) } finally { $es.Dispose() }
                    $bytes = $ms.ToArray(); $ms.Dispose()
                    $want = $listed[$rel]
                    if ($bytes.Length -ne $want.size) { throw "$rel is $($bytes.Length) bytes in the zip and $($want.size) in the payload" }
                    if ((Get-BytesSha256Hex $bytes) -ne $want.sha.ToUpperInvariant()) { throw "$rel in the zip is not the file in the payload" }
                    $seen[$rel] = $true
                }
                foreach ($rel in $listed.Keys) { if (-not $seen.ContainsKey($rel)) { throw "the payload holds $rel, which the zip does not" } }
                '  gate: the {0} files of the payload are the files of {1}, each byte for byte' -f $seen.Count, (Split-Path $relZipPath -Leaf)
            } finally { $zf.Dispose() }
        }
    } finally { $stream.Dispose() }
}
$outHash
