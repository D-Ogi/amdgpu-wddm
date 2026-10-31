#Requires -Version 7.0
# A notes-only repack of a built and gated package (BD-265).
#
#   pwsh -File tools\release\repack-notes.ps1 -Package <Out>\package\<flavour> -Out <new empty dir> [-NotesDir <dir>]
#
# The package ships files\RELEASE-NOTES.md (and RELEASE-NOTES.<lang>.md); the setup window shows them and
# manifest.json lists the SHA-256 and size of each. Notes finished after the package build (gate results, [GATE]
# markers) need a new zip and a new single exe. A rebuild changes the KMD bytes and voids the gates, so this tool
# changes nothing else:
#   1. The old package must be intact: every file of the top folder matches manifest.json and the zip entry of the
#      same path, and the zip has no other entry.
#   2. A copy of the top folder takes the notes of docs\testing\release-notes\<version>.md (and <version>.<lang>.md)
#      by the rules of build-release.ps1 (the four sections, no old setup path). The set of notes files must stay the
#      same. manifest.json changes only in the sha256 and size of each notes entry: the bytes around them stay.
#   3. The proof: the same file list, every file not a notes file or manifest.json byte for byte the old one, and
#      every file against the new manifest.json.
#   4. The zip with the build tool's own Compress-Archive, and the single exe with make-setup-exe.ps1 -Check
#      -ReleaseZip, under the names of the old package. <Out>\repack-report.json holds the old and new hashes.
# build-release-set.ps1 -RepackNotes runs this for each flavour of a set and then the flavour gate and the installer
# dry run on the new package. Exit 0 when the repack and its proof pass; a failure throws before the zip is written.
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Out,
    [string]$NotesDir = (Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'docs\testing\release-notes')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant() }
function Get-StreamHash([IO.Stream]$Stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [Convert]::ToHexString($sha.ComputeHash($Stream)) } finally { $sha.Dispose() }
}
# Relative path (with \) -> SHA-256 of every file under a folder.
function Get-TreeHashes([string]$Top) {
    $map = [ordered]@{}
    foreach ($f in Get-ChildItem -LiteralPath $Top -Recurse -File -Force | Sort-Object FullName) {
        $map[$f.FullName.Substring($Top.Length + 1)] = Get-Hash $f.FullName
    }
    return $map
}

$Package = [IO.Path]::GetFullPath($Package)
$Out = [IO.Path]::GetFullPath($Out)
$tops = @(Get-ChildItem -LiteralPath $Package -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'files\manifest.json') })
if ($tops.Count -ne 1) { throw "$Package holds $($tops.Count) package folders with files\manifest.json, 1 expected" }
$oldTop = $tops[0].FullName
$name = $tops[0].Name
$oldZip = "$oldTop.zip"
if (-not (Test-Path -LiteralPath $oldZip -PathType Leaf)) { throw "no zip $oldZip beside the package folder" }
$oldManifestText = [IO.File]::ReadAllText((Join-Path $oldTop 'files\manifest.json'))
$oldManifest = $oldManifestText | ConvertFrom-Json
$version = [string]$oldManifest.version
if (-not $version -or $name -ne "amdgpu-wddm-tester-$version") { throw "the package folder $name does not name the manifest version '$version'" }
$exes = @(Get-ChildItem -LiteralPath $Package -File -Filter "amdgpu-wddm-setup-$version*.exe")
if ($exes.Count -gt 1) { throw "$Package holds $($exes.Count) single exes of $version" }
$suffix = if ($oldManifest.flavour -eq 'lab') { '-lab' } else { '' }
$exeName = if ($exes.Count) { $exes[0].Name } else { "amdgpu-wddm-setup-$version$suffix.exe" }
if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty" }

# 1. The old package is the gated one: the top folder, its manifest and its zip agree file for file.
$oldFiles = Get-TreeHashes $oldTop
$bad = [Collections.Generic.List[string]]::new()
foreach ($f in $oldManifest.files) {
    $rel = 'files\' + ($f.path -replace '/', '\')
    if (-not $oldFiles.Contains($rel)) { $bad.Add("old package: no $rel"); continue }
    if ($oldFiles[$rel] -ne ([string]$f.sha256).ToUpperInvariant()) { $bad.Add("old package: $rel does not match manifest.json") }
}
$zipRead = [IO.Compression.ZipFile]::OpenRead($oldZip)
try {
    $entries = @($zipRead.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
    foreach ($e in $entries) {
        $rel = ($e.FullName -replace '/', '\')
        if (-not $rel.StartsWith("$name\")) { $bad.Add("old zip: entry $($e.FullName) outside $name"); continue }
        $rel = $rel.Substring($name.Length + 1)
        $s = $e.Open(); try { $h = Get-StreamHash $s } finally { $s.Dispose() }
        if (-not $oldFiles.Contains($rel)) { $bad.Add("old zip: $rel is not in the package folder") }
        elseif ($oldFiles[$rel] -ne $h) { $bad.Add("old zip: $rel differs from the package folder") }
    }
    if ($entries.Count -ne $oldFiles.Count) { $bad.Add("old zip: $($entries.Count) files, the package folder $($oldFiles.Count)") }
} finally { $zipRead.Dispose() }
if ($bad.Count) { $bad | ForEach-Object { "  $_" }; throw "the old package is not intact ($($bad.Count) problems)" }
"old package $name intact: $($oldFiles.Count) files, zip $(Get-Hash $oldZip)"

# 2. The new notes, by the rules of build-release.ps1.
$notes = Join-Path $NotesDir "$version.md"
if (-not (Test-Path -LiteralPath $notes)) { throw "no release notes for ${version}: $notes" }
$notesText = Get-Content -LiteralPath $notes -Raw
foreach ($h in 'New', 'Fixed', 'Known issues', 'Settings affected') {
    if ($notesText -notmatch ('(?m)^## ' + [regex]::Escape($h) + '\s*$')) { throw "release notes $notes lack the section '## $h'" }
}
if ($notesText -match 'setup\\amdgpu_wddm_setup\.exe') { throw "release notes $notes still name setup\amdgpu_wddm_setup.exe" }
$swap = [ordered]@{ 'RELEASE-NOTES.md' = $notes }
foreach ($t in Get-ChildItem -LiteralPath $NotesDir -File -Filter "$version.*.md") {
    $lang = $t.Name.Substring($version.Length + 1) -replace '\.md$', ''
    if ($lang -notmatch '^[a-z]{2}$') { throw "release notes translation $($t.Name): expected <version>.<two-letter language>.md" }
    $swap["RELEASE-NOTES.$lang.md"] = $t.FullName
}
$shipped = @($oldFiles.Keys | Where-Object { $_ -match '^files\\RELEASE-NOTES(\.[a-z]{2})?\.md$' } | ForEach-Object { $_.Substring(6) } | Sort-Object)
$given = @($swap.Keys | Sort-Object)
if (($shipped -join ',') -ne ($given -join ',')) {
    throw "the package ships $($shipped -join ', ') and $NotesDir gives $($given -join ', '): a repack changes notes, it does not add or remove a file"
}
New-Item -ItemType Directory -Force $Out | Out-Null
$newTop = Join-Path $Out $name
Copy-Item -LiteralPath $oldTop -Destination $newTop -Recurse
$manifestText = $oldManifestText
$changes = @()
foreach ($file in $swap.Keys) {
    $dst = Join-Path $newTop "files\$file"
    $entry = @($oldManifest.files | Where-Object { $_.path -ceq $file })
    if ($entry.Count -ne 1) { throw "manifest.json: $($entry.Count) entries of $file, 1 expected" }
    # The values as manifest.json spells them, so that the text replacement and the proof below are exact.
    $old = [ordered]@{ sha256 = [string]$entry[0].sha256; size = [long]$entry[0].size }
    Copy-Item -LiteralPath $swap[$file] -Destination $dst -Force
    $new = [ordered]@{ sha256 = (Get-Hash $dst); size = (Get-Item -LiteralPath $dst).Length }
    # The entry as ConvertTo-Json wrote it: path, sha256 and size in that order. Only the two values change.
    $pattern = '("path":\s*"' + [regex]::Escape($file) + '",\s*"sha256":\s*")' + [regex]::Escape($old.sha256) + '(",\s*"size":\s*)' + $old.size + '(?=\s*[,}\r\n])'
    $hits = [regex]::Matches($manifestText, $pattern)
    if ($hits.Count -ne 1) { throw "manifest.json: $($hits.Count) entries of $file with sha256 $($old.sha256) and size $($old.size), 1 expected" }
    $manifestText = [regex]::Replace($manifestText, $pattern, ('${1}' + $new.sha256 + '${2}' + $new.size))
    $changes += [ordered]@{ file = "files/$file"; source = $swap[$file]; old = $old; new = $new }
    "  $file $($old.sha256.Substring(0, 8))/$($old.size) -> $($new.sha256.Substring(0, 8))/$($new.size)"
}
$manPath = Join-Path $newTop 'files\manifest.json'
[IO.File]::WriteAllText($manPath, $manifestText)

# 3. The proof.
$newFiles = Get-TreeHashes $newTop
$allowed = @(@($swap.Keys | ForEach-Object { "files\$_" }) + 'files\manifest.json')
if (($newFiles.Keys -join '|') -ne ($oldFiles.Keys -join '|')) { $bad.Add('the file list of the new package differs from the old one') }
$changed = @(foreach ($k in $newFiles.Keys) { if ($oldFiles.Contains($k) -and $oldFiles[$k] -ne $newFiles[$k]) { $k } })
foreach ($c in $changed) { if ($allowed -notcontains $c) { $bad.Add("not a notes file, and changed: $c") } }
$newManifest = $manifestText | ConvertFrom-Json
# The new manifest with the old values of the notes entries put back must be the old manifest exactly.
$restored = $manifestText | ConvertFrom-Json
foreach ($c in $changes) {
    $e = @($restored.files | Where-Object { $_.path -ceq $c.file.Substring(6) })
    foreach ($x in $e) { $x.sha256 = $c.old.sha256; $x.size = $c.old.size }
}
$restoredJson = $restored | ConvertTo-Json -Depth 30 -Compress
$oldJson = ($oldManifestText | ConvertFrom-Json) | ConvertTo-Json -Depth 30 -Compress
if ($restoredJson -cne $oldJson) {
    $bad.Add('manifest.json differs from the old one outside the sha256 and size of the notes entries')
}
foreach ($f in $newManifest.files) {
    $rel = 'files\' + ($f.path -replace '/', '\')
    if (-not $newFiles.Contains($rel) -or $newFiles[$rel] -ne ([string]$f.sha256).ToUpperInvariant()) { $bad.Add("new package: $rel does not match manifest.json") }
    elseif ((Get-Item -LiteralPath (Join-Path $newTop $rel)).Length -ne [long]$f.size) { $bad.Add("new package: $rel does not have the size of manifest.json") }
}
foreach ($f in @($oldManifest.files)) {
    $n = @($newManifest.files | Where-Object { $_.path -ceq $f.path })
    $isNotes = $allowed -contains ('files\' + ($f.path -replace '/', '\'))
    if ($n.Count -ne 1) { $bad.Add("manifest.json: entry $($f.path) lost or doubled"); continue }
    if (-not $isNotes -and ($n[0].sha256 -ne $f.sha256 -or [long]$n[0].size -ne [long]$f.size)) { $bad.Add("manifest.json: entry $($f.path) changed") }
}
if ($bad.Count) { $bad | ForEach-Object { "  $_" }; throw "the repack proof fails ($($bad.Count) problems)" }
"proof: $($newFiles.Count) files, $($newFiles.Count - $changed.Count) byte for byte the old ones, changed: $($changed -join ', ')"

# 4. The zip and the single exe, as build-release.ps1 makes them.
$zip = "$newTop.zip"
Compress-Archive -Path $newTop -DestinationPath $zip
"zip $zip SHA256 $(Get-Hash $zip)"
$exe = Join-Path $Out $exeName
& (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $newTop -Out $exe -Check -ReleaseZip $zip | ForEach-Object { "  $_" }
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "make-setup-exe.ps1 wrote no $exe" }
"single exe $exe SHA256 $(Get-Hash $exe)"
$report = [ordered]@{ tool = 'repack-notes'; utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'); version = $version
    flavour = $oldManifest.flavour; from = $Package; out = $Out; unchanged_files = $newFiles.Count - $changed.Count; notes = $changes
    manifest = [ordered]@{ old = $oldFiles['files\manifest.json']; new = $newFiles['files\manifest.json'] }
    zip = [ordered]@{ old = (Get-Hash $oldZip); new = (Get-Hash $zip); file = $zip }
    exe = [ordered]@{ old = $(if ($exes.Count) { Get-Hash $exes[0].FullName }); new = (Get-Hash $exe); file = $exe } }
[IO.File]::WriteAllText((Join-Path $Out 'repack-report.json'), ($report | ConvertTo-Json -Depth 6))
"repack-notes: ok ($Out\repack-report.json)"
