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
#      -ReleaseZip, under the names of the old package. The single exe is then signed as build-release.ps1 signs it,
#      by the signer of the old single exe (BD-294: up to tester.36 the repack left the exe unsigned), and
#      make-setup-exe.ps1 -CheckSigned reads the signed bytes back.
#   5. The self-update assets of a release flavour package (BD-294): nagi-update.json pins the sha256 and size of the
#      driver zip and the sha256 of files\manifest.json, so the old manifest names the old zip. The repack copies the
#      apps pack (no notes file is in it), writes nagi-update.json again over the new zip with nagi_update.py, and
#      proves that only those three values changed. It signs it (signtool /p7, wrapped as a ContentInfo) and checks
#      the signature in Windows PowerShell 5.1 with the pinned list of the package's own installer\nagi-update.ps1.
#   <Out>\repack-report.json holds the old and new hashes.
# build-release-set.ps1 -RepackNotes runs this for each flavour of a set and then the flavour gate and the installer
# dry run on the new package. Exit 0 when the repack and its proof pass; a failure throws before the zip is written.
# The signing inputs are those of build-release.ps1: the PFX, its password file and the test certificate of
# secrets\release, or a store certificate (-UserModeSignerThumbprint with -TimestampUrl).
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Out,
    [string]$NotesDir = (Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'docs\testing\release-notes'),
    [Parameter(Mandatory)][string]$SignTool,
    [Parameter(Mandatory)][string]$Pfx,
    [Parameter(Mandatory)][string]$PasswordFile,
    [Parameter(Mandatory)][string]$TestCertificate,
    [string]$UserModeSignerThumbprint,
    [string]$TimestampUrl
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'user-mode-signing.ps1')
foreach ($f in $SignTool, $Pfx, $PasswordFile, $TestCertificate) { if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "missing signing input $f" } }
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
# The lab flavour has the suffix -lab in the names of its folder, zip and single exe (build-release.ps1).
$suffix = if ($oldManifest.flavour -eq 'lab') { '-lab' } else { '' }
if (-not $version -or $name -ne "amdgpu-wddm-tester-$version$suffix") { throw "the package folder $name does not name the manifest version '$version' of the $($oldManifest.flavour) flavour" }
$exes = @(Get-ChildItem -LiteralPath $Package -File -Filter "amdgpu-wddm-setup-$version*.exe")
if ($exes.Count -gt 1) { throw "$Package holds $($exes.Count) single exes of $version" }
$exeName = if ($exes.Count) { $exes[0].Name } else { "amdgpu-wddm-setup-$version$suffix.exe" }
if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "$Out is not empty" }

# The signer: the one build-release.ps1 selects for this flavour, and the signer of the old single exe.
$testThumb = (New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $TestCertificate).Thumbprint
$signer = Get-UserModeSigner -Thumbprint $UserModeSignerThumbprint -TimestampUrl $TimestampUrl -Flavour ([string]$oldManifest.flavour) -TestThumbprint $testThumb
if ($exes.Count) {
    $oldSig = Get-AuthenticodeSignature -LiteralPath $exes[0].FullName
    $oldSigner = $(if ($oldSig.SignerCertificate) { [string]$oldSig.SignerCertificate.Thumbprint } else { '' })
    if (-not $oldSigner) { throw "the old single exe $($exes[0].Name) is not signed (status $($oldSig.Status)): not a package of build-release.ps1" }
    if ($oldSigner -ne $signer.thumbprint) { throw "the old single exe is signed by $oldSigner and the repack signer is $($signer.thumbprint)" }
}
"signer: $($signer.mode) $($signer.thumbprint)"

# The self-update assets beside the old package (release flavour only), checked before anything is written.
$oldUpdate = Join-Path $Package 'nagi-update.json'
$update = $null
if (Test-Path -LiteralPath $oldUpdate -PathType Leaf) {
    if ($oldManifest.flavour -ne 'release') { throw "$oldUpdate lies beside a $($oldManifest.flavour) package: only the release flavour carries the self-update assets" }
    $oldUpdateText = [IO.File]::ReadAllText($oldUpdate)
    $oldUpdateJson = $oldUpdateText | ConvertFrom-Json
    if ([string]$oldUpdateJson.release -ne $version) { throw "$oldUpdate names release $($oldUpdateJson.release), the package is $version" }
    $oldDriver = @($oldUpdateJson.components | Where-Object { $_.kind -eq 'driver' })
    $oldApps = @($oldUpdateJson.components | Where-Object { $_.kind -eq 'apps' })
    if ($oldDriver.Count -ne 1 -or $oldApps.Count -ne 1) { throw "${oldUpdate}: one driver and one apps component expected" }
    if ([string]$oldDriver[0].sha256 -ne (Get-Hash $oldZip)) { throw "$oldUpdate does not pin the old zip $oldZip" }
    $appsPack = Join-Path $Package ([string]$oldApps[0].asset)
    if (-not (Test-Path -LiteralPath $appsPack -PathType Leaf) -or (Get-Hash $appsPack) -ne [string]$oldApps[0].sha256) {
        throw "the apps pack $appsPack is missing or does not match $oldUpdate"
    }
    if (-not (Test-Path -LiteralPath "$oldUpdate.p7s" -PathType Leaf)) { throw "no signature $oldUpdate.p7s beside the old manifest" }
    $update = [ordered]@{ text = $oldUpdateText; json = $oldUpdateJson; apps = $appsPack }
} elseif ($oldManifest.flavour -eq 'release' -and (Test-Path -LiteralPath (Join-Path $oldTop 'files\installer\nagi-update.ps1'))) {
    throw "the release package $name ships the self-update but $Package has no nagi-update.json"
}
$pass = (Get-Content -LiteralPath $PasswordFile -Raw).Trim()
function Invoke-SignTool([string[]]$Arguments, [string]$What) {
    $text = (& $SignTool @Arguments 2>&1 | ForEach-Object { [string]$_ }) -join ' / '
    if ($LASTEXITCODE -ne 0) { throw "signtool $What failed: $($text -replace [regex]::Escape($pass), '<redacted>')" }
}

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
# The single file is signed as a whole after it is written, as build-release.ps1 signs it (BD-160), and -CheckSigned
# proves that the signed file is the checked file plus its certificate table.
Invoke-SignTool (Get-SignToolSignArguments -Signer $signer -Path $exe -Pfx $Pfx -Password $pass) "sign $exe"
Test-UserModeSignature -Signer $signer -Path $exe
& (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $newTop -Out $exe -CheckSigned -ReleaseZip $zip | ForEach-Object { "  $_" }
"single exe $exe SHA256 $(Get-Hash $exe), signed by $($signer.thumbprint)"

# 5. The self-update assets of a release flavour package (BD-294).
$updateReport = $null
if ($update) {
    $newApps = Join-Path $Out (Split-Path -Leaf $update.apps)
    Copy-Item -LiteralPath $update.apps -Destination $newApps
    $newUpdate = Join-Path $Out 'nagi-update.json'
    $python = (Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $text = & $python -I (Join-Path $PSScriptRoot 'nagi_update.py') manifest --package (Join-Path $newTop 'files') --release-zip $zip `
        --apps-pack $newApps --min-installed ([string]$update.json.min_installed) --out $newUpdate 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "nagi_update.py manifest failed: $text" }
    # The proof: the new manifest names the new zip and the new files\manifest.json, and with the old values of these
    # three fields put back it is the old manifest exactly.
    $newJson = [IO.File]::ReadAllText($newUpdate) | ConvertFrom-Json
    $d = @($newJson.components | Where-Object { $_.kind -eq 'driver' })[0]
    if ([string]$d.sha256 -ne (Get-Hash $zip) -or [long]$d.size -ne (Get-Item -LiteralPath $zip).Length) { $bad.Add('nagi-update.json does not pin the new zip') }
    if ([string]$d.driver.package_manifest_sha256 -ne (Get-Hash $manPath)) { $bad.Add('nagi-update.json does not pin the new files\manifest.json') }
    $o = @($update.json.components | Where-Object { $_.kind -eq 'driver' })[0]
    $d.sha256 = $o.sha256; $d.size = $o.size; $d.driver.package_manifest_sha256 = $o.driver.package_manifest_sha256
    if (($newJson | ConvertTo-Json -Depth 30 -Compress) -cne ($update.json | ConvertTo-Json -Depth 30 -Compress)) {
        $bad.Add('nagi-update.json differs from the old one outside the zip sha256, the zip size and the package manifest sha256')
    }
    if ($bad.Count) { $bad | ForEach-Object { "  $_" }; throw "the update manifest proof fails ($($bad.Count) problems)" }
    $p7Dir = Join-Path $Out 'nagi-update-p7'
    [void][IO.Directory]::CreateDirectory($p7Dir)
    Invoke-SignTool (Get-ManifestSignToolArguments -Signer $signer -Path $newUpdate -OutDir $p7Dir -Pfx $Pfx -Password $pass) "/p7 $newUpdate"
    $newSignature = "$newUpdate.p7s"
    [IO.File]::WriteAllBytes($newSignature, (ConvertTo-CmsContentInfo ([IO.File]::ReadAllBytes((Join-Path $p7Dir 'nagi-update.json.p7')))))
    Remove-Item -LiteralPath $p7Dir -Recurse -Force
    # The check of the installed release: Windows PowerShell 5.1 and the pinned list of the package's own installer.
    $q = { param($s) "'" + $s.Replace("'", "''") + "'" }
    $check = "& { . $(& $q (Join-Path $newTop 'files\installer\nagi-update.ps1')); `$r = Test-NagiManifestSignature -ManifestPath $(& $q $newUpdate) -SignaturePath $(& $q $newSignature); " +
        "if (`$r.ok) { 'signer ' + `$r.thumbprint + ' is pinned'; exit 0 }; `$r.reason; exit 1 }"
    $text = & (Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $check 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "the update manifest signature does not verify as the installed release checks it: $($text.Trim())" }
    "update manifest $newUpdate SHA256 $(Get-Hash $newUpdate), $($text.Trim())"
    $updateReport = [ordered]@{ manifest = [ordered]@{ old = (Get-Hash $oldUpdate); new = (Get-Hash $newUpdate); file = $newUpdate }
        signature = $newSignature; apps = [ordered]@{ sha256 = (Get-Hash $newApps); file = $newApps } }
}
$pass = $null
$report = [ordered]@{ tool = 'repack-notes'; utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'); version = $version
    flavour = $oldManifest.flavour; from = $Package; out = $Out; unchanged_files = $newFiles.Count - $changed.Count; notes = $changes
    manifest = [ordered]@{ old = $oldFiles['files\manifest.json']; new = $newFiles['files\manifest.json'] }
    zip = [ordered]@{ old = (Get-Hash $oldZip); new = (Get-Hash $zip); file = $zip }
    exe = [ordered]@{ old = $(if ($exes.Count) { Get-Hash $exes[0].FullName }); new = (Get-Hash $exe); file = $exe; signer = $signer.thumbprint }
    update = $updateReport }
[IO.File]::WriteAllText((Join-Path $Out 'repack-report.json'), ($report | ConvertTo-Json -Depth 6))
"repack-notes: ok ($Out\repack-report.json)"
