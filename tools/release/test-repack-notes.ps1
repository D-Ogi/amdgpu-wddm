#Requires -Version 7.0
# Host test of repack-notes.ps1 (BD-265) on a small package of its own in the layout of a release: setup.exe and
# files\ (manifest.json, the notes, a translation and two payload files), its zip and its single exe. The repack must
# change the notes and their two manifest values only, every other file byte for byte, and refuse a package or notes
# that do not fit.
#   pwsh -File tools\release\test-repack-notes.ps1 -Out <dir>     (the 'repack-notes' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$tool = Join-Path $PSScriptRoot 'repack-notes.ps1'
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('repack-notes-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$version = '0.7.216.999-tester.99'
$name = "amdgpu-wddm-tester-$version"
$passed = 0
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Write-Bytes([string]$Path, [byte[]]$Bytes) { New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null; [IO.File]::WriteAllBytes($Path, $Bytes) }
$notesBody = "# $version`n`n## New`n- a`n`n## Fixed`n- b`n`n## Known issues`n- c`n`n## Settings affected`n- none`n"
function Write-Notes([string]$Dir, [string]$Tag) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $Dir "$version.md"), $notesBody.Replace('- a', "- a ($Tag)"))
    [IO.File]::WriteAllText((Join-Path $Dir "$version.pl.md"), $notesBody.Replace('- a', "- a ($Tag, po polsku)"))
}
# The package as build-release.ps1 lays it out, with its zip and single exe in $Dir.
function New-Package([string]$Dir) {
    $top = Join-Path $Dir $name
    $files = Join-Path $top 'files'
    $rng = [Random]::new(265)
    $bytes = [byte[]]::new(70000); $rng.NextBytes($bytes)
    # make-setup-exe.ps1 reads the PE security directory of the stub (BD-160): a PE32+ header with an empty
    # certificate table, so the stub counts as an unsigned program image.
    [Array]::Clear($bytes, 0, 0x200)
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int32]0x80).CopyTo($bytes, 0x3C)
    [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($bytes, 0x80)
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($bytes, 0x98)
    [BitConverter]::GetBytes([uint32]16).CopyTo($bytes, 0x98 + 108)
    Write-Bytes (Join-Path $top 'setup.exe') $bytes
    $bytes = [byte[]]::new(5000); $rng.NextBytes($bytes)
    Write-Bytes (Join-Path $files 'payload\desktop\a.dll') $bytes
    Write-Bytes (Join-Path $files 'kmd\bc250kmd.sys') ([Text.Encoding]::ASCII.GetBytes('driver bytes that the gates saw'))
    Write-Notes (Join-Path $Dir 'notes-old') 'candidate'
    Copy-Item (Join-Path $Dir "notes-old\$version.md") (Join-Path $files 'RELEASE-NOTES.md')
    Copy-Item (Join-Path $Dir "notes-old\$version.pl.md") (Join-Path $files 'RELEASE-NOTES.pl.md')
    $list = @(foreach ($f in Get-ChildItem -LiteralPath $files -Recurse -File | Sort-Object FullName) {
        [ordered]@{ path = $f.FullName.Substring($files.Length + 1) -replace '\\', '/'; sha256 = (Get-Hash $f.FullName); size = $f.Length }
    })
    $setup = Join-Path $top 'setup.exe'
    $manifest = [ordered]@{ schema = 1; flavour = 'release'; name = $name; release = $version; version = $version
        driver_signing = [ordered]@{ submitted_payload_sha256 = ('AB' * 32) }
        setup = [ordered]@{ file = 'setup.exe'; sha256 = (Get-Hash $setup); size = (Get-Item $setup).Length }; files = $list }
    [IO.File]::WriteAllText((Join-Path $files 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8))
    Compress-Archive -Path $top -DestinationPath "$top.zip"
    & (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $top -Out (Join-Path $Dir "amdgpu-wddm-setup-$version.exe") -Check -ReleaseZip "$top.zip" *> $null
    return $top
}
function Invoke-Repack([string]$Package, [string]$To, [string]$Notes) {
    $text = & pwsh -NoProfile -File $tool -Package $Package -Out $To -NotesDir $Notes 2>&1 | Out-String
    # The error view of the child wraps long lines with "|" margins: one line, single spaces.
    return [pscustomobject]@{ code = $LASTEXITCODE; text = (($text -replace '\s*\|\s*', ' ') -replace '\s+', ' ') }
}

$pkgDir = Join-Path $base 'package\release'
$oldTop = New-Package $pkgDir
$newNotes = Join-Path $base 'notes-final'
Write-Notes $newNotes 'final'

# The repack.
$r = Invoke-Repack $pkgDir (Join-Path $base 'repack') $newNotes
Assert ($r.code -eq 0) "the repack passes: $($r.text)"
$newTop = Join-Path $base "repack\$name"
$oldFiles = @(Get-ChildItem -LiteralPath $oldTop -Recurse -File | ForEach-Object { $_.FullName.Substring($oldTop.Length + 1) } | Sort-Object)
$newFiles = @(Get-ChildItem -LiteralPath $newTop -Recurse -File | ForEach-Object { $_.FullName.Substring($newTop.Length + 1) } | Sort-Object)
Assert (($oldFiles -join '|') -eq ($newFiles -join '|')) 'the same file list'
$notesFiles = @('files\RELEASE-NOTES.md', 'files\RELEASE-NOTES.pl.md')
foreach ($f in $oldFiles) {
    $same = (Get-Hash (Join-Path $oldTop $f)) -eq (Get-Hash (Join-Path $newTop $f))
    if ($f -in $notesFiles -or $f -eq 'files\manifest.json') { Assert (-not $same) "$f changed" }
    else { Assert $same "$f byte for byte the old one" }
}
Assert ((Get-Hash (Join-Path $newTop 'files\RELEASE-NOTES.md')) -eq (Get-Hash (Join-Path $newNotes "$version.md"))) 'the new notes are the given ones'
# manifest.json: only the two values of each notes entry differ, as text.
$oldText = [IO.File]::ReadAllText((Join-Path $oldTop 'files\manifest.json'))
$newText = [IO.File]::ReadAllText((Join-Path $newTop 'files\manifest.json'))
foreach ($f in $notesFiles) {
    $p = Join-Path $oldTop $f; $q = Join-Path $newTop $f
    $oldText = [regex]::Replace($oldText.Replace((Get-Hash $p), (Get-Hash $q)), "(""size"":\s*)$((Get-Item $p).Length)\b", "`${1}$((Get-Item $q).Length)")
}
Assert ($oldText -ceq $newText) 'manifest.json changes in the notes values only'
# The zip holds the new package folder, and the single exe passed make-setup-exe -Check -ReleaseZip.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $base "repack\$name.zip"))
try {
    $entry = $zip.GetEntry("$name/files/RELEASE-NOTES.md")
    $s = $entry.Open(); $reader = [IO.StreamReader]::new($s); $zipNotes = $reader.ReadToEnd(); $reader.Dispose()
    Assert ($zipNotes.Contains('(final)')) 'the zip carries the new notes'
    Assert (@($zip.Entries | Where-Object { -not $_.FullName.EndsWith('/') }).Count -eq $newFiles.Count) 'the zip has every file once'
} finally { $zip.Dispose() }
Assert (Test-Path -LiteralPath (Join-Path $base "repack\amdgpu-wddm-setup-$version.exe")) 'the single exe has the old name'
Assert ($r.text -match 'single-file setup') 'make-setup-exe ran'
$report = Get-Content -LiteralPath (Join-Path $base 'repack\repack-report.json') -Raw | ConvertFrom-Json
Assert ($report.unchanged_files -eq ($newFiles.Count - 3) -and $report.notes.Count -eq 2) 'the report counts the unchanged files'
Assert ($report.zip.old -ne $report.zip.new -and $report.exe.old -and $report.exe.old -ne $report.exe.new) 'the report gives the old and new zip and exe'

# Refusals.
$r = Invoke-Repack $pkgDir (Join-Path $base 'repack') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'is not empty') 'an output folder that is not empty'
$bad = Join-Path $base 'notes-bad'
Write-Notes $bad 'bad'
[IO.File]::WriteAllText((Join-Path $bad "$version.md"), $notesBody.Replace("## Known issues`n- c`n`n", ''))
$r = Invoke-Repack $pkgDir (Join-Path $base 'r-bad') $bad
Assert ($r.code -ne 0 -and $r.text -match "lack the section '## Known issues'") "notes without a section: $($r.code) $($r.text)"
$extra = Join-Path $base 'notes-extra'
Write-Notes $extra 'extra'
[IO.File]::WriteAllText((Join-Path $extra "$version.ja.md"), $notesBody)
$r = Invoke-Repack $pkgDir (Join-Path $base 'r-extra') $extra
Assert ($r.code -ne 0 -and $r.text -match 'does not add or remove a file') 'a new translation is not a notes-only repack'
Assert (-not (Test-Path (Join-Path $base "r-extra\$name.zip"))) 'no zip after a refusal'
# A package folder that is not the zip that was gated.
[IO.File]::WriteAllText((Join-Path $oldTop 'files\kmd\bc250kmd.sys'), 'other driver bytes')
$r = Invoke-Repack $pkgDir (Join-Path $base 'r-tampered') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'not intact') 'a changed file of the old package'
Assert ($r.text -match 'kmd\\bc250kmd.sys does not match manifest.json') 'the problem names the file'

# build-release-set.ps1 -RepackNotes: the repack of each flavour with this tree's committed notes, then the gates of a
# package. The fixture takes the version of the newest notes file of the tree, so the repack passes; its payload has
# no real PE images, so the flavour gate must then stop the run.
$repo = Split-Path (Split-Path $PSScriptRoot)
$notesDir = Join-Path $repo 'docs\testing\release-notes'
$latest = Get-ChildItem -LiteralPath $notesDir -File -Filter '*.md' | Where-Object { $_.BaseName -match '^\d+\.\d+\.\d+\.\d+-tester\.\d+$' } |
    Sort-Object { [int]($_.BaseName -replace '^.*tester\.', '') } | Select-Object -Last 1
$dirtyNotes = @(& git -C $repo status --porcelain -- docs/testing/release-notes).Count
if (-not $latest -or $dirtyNotes) {
    "SKIP: -RepackNotes orchestration (no tester notes in $notesDir, or they are not committed)"
} else {
    $version = $latest.BaseName
    $name = "amdgpu-wddm-tester-$version"
    $set = Join-Path $base 'set'
    $null = New-Package (Join-Path $set 'package\release')
    # The fixture ships the same notes files as the tree gives for this version.
    $files = Join-Path $set "package\release\$name\files"
    Remove-Item -LiteralPath (Join-Path $files 'RELEASE-NOTES.pl.md')
    foreach ($t in Get-ChildItem -LiteralPath $notesDir -File -Filter "$version.*.md") {
        Copy-Item $t.FullName (Join-Path $files ("RELEASE-NOTES." + $t.Name.Substring($version.Length + 1)))
    }
    $list = @(foreach ($f in Get-ChildItem -LiteralPath $files -Recurse -File | Where-Object Name -ne 'manifest.json' | Sort-Object FullName) {
        [ordered]@{ path = $f.FullName.Substring($files.Length + 1) -replace '\\', '/'; sha256 = (Get-Hash $f.FullName); size = $f.Length }
    })
    $m = Get-Content -LiteralPath (Join-Path $files 'manifest.json') -Raw | ConvertFrom-Json
    $m.files = $list
    [IO.File]::WriteAllText((Join-Path $files 'manifest.json'), ($m | ConvertTo-Json -Depth 8))
    $top = Join-Path $set "package\release\$name"
    Remove-Item -LiteralPath "$top.zip", (Join-Path $set "package\release\amdgpu-wddm-setup-$version.exe")
    Compress-Archive -Path $top -DestinationPath "$top.zip"
    $text = (& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build-release-set.ps1') -RepackNotes -From $set -Flavours release 2>&1 | Out-String) -replace '\s+', ' '
    Assert ($LASTEXITCODE -eq 1 -and $text -match 'the flavour gate of the repacked release package fails') "-RepackNotes stops at the flavour gate: $text"
    $log = Get-Content -LiteralPath (Join-Path $set 'logs\repack-release\repack.log') -Raw
    Assert ($log -match 'repack-notes: ok') 'the repack step of -RepackNotes passed'
    Assert (Test-Path -LiteralPath (Join-Path $set "package-notes\release\$name.zip")) '-RepackNotes writes the new zip under package-notes'
    $rr = Get-Content -LiteralPath (Join-Path $set 'repack-report.json') -Raw | ConvertFrom-Json
    Assert ($rr.status -eq 'failed' -and $rr.notes_commit) 'the report of -RepackNotes names the notes commit and the failure'
}

Remove-Item -LiteralPath $base -Recurse -Force
"PASS: notes-only repack, $passed checks (BD-265)"
