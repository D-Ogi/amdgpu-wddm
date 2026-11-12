#Requires -Version 7.0
# Host test of repack-notes.ps1 (BD-265, BD-294) on a small package of its own in the layout of a release: setup.exe
# and files\ (manifest.json, the notes, a translation and two payload files), its zip and its signed single exe. The
# repack must change the notes and their two manifest values only, every other file byte for byte, sign the new single
# exe with the signer of the old one, and refuse a package or notes that do not fit. A second package carries the
# self-update assets of a release flavour (nagi-update.json, its signature and the apps pack): the repack must write
# the update manifest again over the new zip, change only the zip and package manifest values in it, and sign it so
# that the package's own installer\nagi-update.ps1 accepts it.
# A code-signing certificate of this run signs everything (no certificate store is opened); its PFX, password file and
# certificate lie under -Out for the run and are deleted at the end. The stub of the single exe is a copy of
# %windir%\System32\hostname.exe, a small program without an embedded signature that signtool can sign.
#   pwsh -File tools\release\test-repack-notes.ps1 -Out <dir> [-SignTool <signtool.exe>]   (the 'repack-notes' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out, [string]$SignTool)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'user-mode-signing.ps1')
$tool = Join-Path $PSScriptRoot 'repack-notes.ps1'
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('repack-notes-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$version = '0.7.216.999-tester.99'
$name = "amdgpu-wddm-tester-$version"
$passed = 0
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Write-Bytes([string]$Path, [byte[]]$Bytes) { New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null; [IO.File]::WriteAllBytes($Path, $Bytes) }
if (-not $SignTool -and $env:BC250_ROOT) { $SignTool = Join-Path $env:BC250_ROOT 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe' }
if (-not $SignTool) { $SignTool = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot)))) 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe' }
if (-not (Test-Path -LiteralPath $SignTool -PathType Leaf)) { throw "no signtool at ${SignTool}: pass -SignTool or set BC250_ROOT" }
$stub = Join-Path $env:windir 'System32\hostname.exe'

# The signers of this run: the publisher (PFX, password file and certificate, as secrets\release holds them) and a
# stranger.
function New-RunCert([string]$CommonName) {
    $rsa = [Security.Cryptography.RSA]::Create(2048)
    $req = [Security.Cryptography.X509Certificates.CertificateRequest]::new("CN=$CommonName", $rsa,
        [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $eku = [Security.Cryptography.OidCollection]::new(); [void]$eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
    $req.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
    return $req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddYears(5))
}
function New-SecretsDir([string]$Dir, $Cert) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $pw = [Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllBytes((Join-Path $Dir 'amdgpu-wddm-release.pfx'), $Cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $pw))
    [IO.File]::WriteAllText((Join-Path $Dir 'amdgpu-wddm-release.pass'), $pw)
    [IO.File]::WriteAllBytes((Join-Path $Dir 'amdgpu-wddm-release.cer'), $Cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    return [pscustomobject]@{ dir = $Dir; thumbprint = $Cert.Thumbprint; pfx = (Join-Path $Dir 'amdgpu-wddm-release.pfx')
        pass = (Join-Path $Dir 'amdgpu-wddm-release.pass'); cer = (Join-Path $Dir 'amdgpu-wddm-release.cer') }
}
$publisher = New-SecretsDir (Join-Path $base 'secrets') (New-RunCert 'Nagi repack test publisher')
$stranger = New-SecretsDir (Join-Path $base 'secrets-stranger') (New-RunCert 'Nagi repack test stranger')
function Invoke-RunSign([string]$Path, $Secrets) {
    $signer = [pscustomobject]@{ mode = 'test'; thumbprint = $Secrets.thumbprint; timestamp_url = $null; store = $null; subject = $null }
    $a = Get-SignToolSignArguments -Signer $signer -Path $Path -Pfx $Secrets.pfx -Password (Get-Content -LiteralPath $Secrets.pass -Raw)
    $text = (& $SignTool @a 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "signtool sign $Path failed: $text" }
}
function Get-Signer([string]$Path) { $s = Get-AuthenticodeSignature -LiteralPath $Path; if ($s.SignerCertificate) { $s.SignerCertificate.Thumbprint } else { '' } }

$notesBody = "# $version`n`n## New`n- a`n`n## Fixed`n- b`n`n## Known issues`n- c`n`n## Settings affected`n- none`n"
function Write-Notes([string]$Dir, [string]$Tag) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $Dir "$version.md"), $notesBody.Replace('- a', "- a ($Tag)"))
    [IO.File]::WriteAllText((Join-Path $Dir "$version.pl.md"), $notesBody.Replace('- a', "- a ($Tag, po polsku)"))
}
function Write-Manifest([string]$Files, [string]$Top, [hashtable]$Extra = @{}, [string]$Flavour = 'release') {
    $list = @(foreach ($f in Get-ChildItem -LiteralPath $Files -Recurse -File | Where-Object Name -ne 'manifest.json' | Sort-Object FullName) {
        [ordered]@{ path = $f.FullName.Substring($Files.Length + 1) -replace '\\', '/'; sha256 = (Get-Hash $f.FullName); size = $f.Length }
    })
    $setup = Join-Path $Top 'setup.exe'
    $manifest = [ordered]@{ schema = 1; flavour = $Flavour; name = $name; release = $version; version = $version
        driver_signing = [ordered]@{ submitted_payload_sha256 = ('AB' * 32) }
        setup = [ordered]@{ file = 'setup.exe'; sha256 = (Get-Hash $setup); size = (Get-Item $setup).Length } }
    foreach ($k in $Extra.Keys) { $manifest[$k] = $Extra[$k] }
    $manifest.files = $list
    [IO.File]::WriteAllText((Join-Path $Files 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8))
}
# The zip and the signed single exe of a package folder, as build-release.ps1 makes them.
function Complete-Package([string]$Dir, [string]$Top, $Secrets, [string]$Suffix = '') {
    Compress-Archive -Path $Top -DestinationPath "$Top.zip"
    $exe = Join-Path $Dir "amdgpu-wddm-setup-$version$Suffix.exe"
    & (Join-Path $PSScriptRoot 'make-setup-exe.ps1') -PackageTop $Top -Out $exe -Check -ReleaseZip "$Top.zip" *> $null
    if ($Secrets) { Invoke-RunSign $exe $Secrets }
}
# The package as build-release.ps1 lays it out, with its zip and single exe in $Dir. -Update adds the files of the
# self-update (payload/control, the state catalog, installer\nagi-update.ps1 pinning -Pin) and update_components.
function New-Package([string]$Dir, $Secrets = $publisher, [switch]$Update, [string]$Pin, [string]$Flavour = 'release') {
    # The lab flavour has the suffix -lab in the names of its folder, zip and single exe.
    $suffix = if ($Flavour -eq 'lab') { '-lab' } else { '' }
    $top = Join-Path $Dir "$name$suffix"
    $files = Join-Path $top 'files'
    $rng = [Random]::new(265)
    New-Item -ItemType Directory -Force $top | Out-Null
    Copy-Item -LiteralPath $stub -Destination (Join-Path $top 'setup.exe')
    $bytes = [byte[]]::new(5000); $rng.NextBytes($bytes)
    Write-Bytes (Join-Path $files 'payload\desktop\a.dll') $bytes
    Write-Bytes (Join-Path $files 'kmd\bc250kmd.sys') ([Text.Encoding]::ASCII.GetBytes('driver bytes that the gates saw'))
    Write-Notes (Join-Path $Dir 'notes-old') 'candidate'
    Copy-Item (Join-Path $Dir "notes-old\$version.md") (Join-Path $files 'RELEASE-NOTES.md')
    Copy-Item (Join-Path $Dir "notes-old\$version.pl.md") (Join-Path $files 'RELEASE-NOTES.pl.md')
    $extra = @{}
    if ($Update) {
        Write-Bytes (Join-Path $files 'payload\control\app.json') ([Text.Encoding]::ASCII.GetBytes('{"app": 1}'))
        $state = [byte[]]::new(80)
        [Text.Encoding]::ASCII.GetBytes("AMDST02`0").CopyTo($state, 0)
        [BitConverter]::GetBytes([uint32]2).CopyTo($state, 8)
        [Text.Encoding]::ASCII.GetBytes('c' * 64).CopyTo($state, 16)
        Write-Bytes (Join-Path $files 'payload\kmd\amdgpu-state.bin') $state
        $nagi = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'installer\nagi-update.ps1'))
        $pinned = "`$script:NagiPinnedSigners = @('$Pin')"
        $nagi2 = [regex]::Replace($nagi, '(?m)^\$script:NagiPinnedSigners = @\([^)]*\)', [Text.RegularExpressions.MatchEvaluator] { param($m) $pinned })
        if ($nagi2 -eq $nagi) { throw 'the fixture could not set the pinned signer of installer\nagi-update.ps1' }
        Write-Bytes (Join-Path $files 'installer\nagi-update.ps1') ([Text.UTF8Encoding]::new($true).GetBytes($nagi2))
        $extra = @{ kmd_abi = '0x000700D8'; kmd_version = '40.7.216.999'; built_utc = '2026-11-04T12:00:00Z'
            update_components = [ordered]@{ driver = [ordered]@{ content_id = ('D' * 64); files = 3 }; apps = [ordered]@{ content_id = ('A' * 64); files = 1 } } }
    }
    Write-Manifest $files $top $extra $Flavour
    Complete-Package $Dir $top $Secrets $suffix
    if ($Update) {
        $py = (Get-Command python -CommandType Application | Select-Object -First 1).Source
        $r = & $py -I (Join-Path $PSScriptRoot 'nagi_update.py') packs --package $files --release $version --out $Dir 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "fixture packs: $r" }
        $r = & $py -I (Join-Path $PSScriptRoot 'nagi_update.py') manifest --package $files --release-zip "$top.zip" --apps-pack (Join-Path $Dir "nagi-apps-$version.zip") `
            --min-installed '0.7.216.104-tester.32' --out (Join-Path $Dir 'nagi-update.json') 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "fixture manifest: $r" }
        [IO.File]::WriteAllBytes((Join-Path $Dir 'nagi-update.json.p7s'), [byte[]](1, 2, 3))   # the repack makes a new one
    }
    return $top
}
function Invoke-Repack([string]$Package, [string]$To, [string]$Notes, $Secrets = $publisher) {
    $text = & pwsh -NoProfile -File $tool -Package $Package -Out $To -NotesDir $Notes -SignTool $SignTool -Pfx $Secrets.pfx `
        -PasswordFile $Secrets.pass -TestCertificate $Secrets.cer 2>&1 | Out-String
    # The error view of the child wraps long lines with "|" margins: one line, single spaces.
    return [pscustomobject]@{ code = $LASTEXITCODE; text = (($text -replace '\s*\|\s*', ' ') -replace '\s+', ' ') }
}

$pkgDir = Join-Path $base 'package\release'
$oldTop = New-Package $pkgDir
$newNotes = Join-Path $base 'notes-final'
Write-Notes $newNotes 'final'
Assert ((Get-Signer (Join-Path $pkgDir "amdgpu-wddm-setup-$version.exe")) -eq $publisher.thumbprint) 'the fixture single exe is signed'

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
$newExe = Join-Path $base "repack\amdgpu-wddm-setup-$version.exe"
Assert (Test-Path -LiteralPath $newExe) 'the single exe has the old name'
Assert ($r.text -match 'single-file setup') 'make-setup-exe ran'
# BD-294: the new single exe carries the signature of the old one's signer, and -CheckSigned read it back.
$sig = Get-AuthenticodeSignature -LiteralPath $newExe
Assert ($sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $publisher.thumbprint) "the new single exe is signed by the publisher (status $($sig.Status))"
Assert ([string]$sig.Status -notin 'NotSigned', 'HashMismatch') "the signature of the new single exe covers its bytes (status $($sig.Status))"
$report = Get-Content -LiteralPath (Join-Path $base 'repack\repack-report.json') -Raw | ConvertFrom-Json
Assert ($report.unchanged_files -eq ($newFiles.Count - 3) -and $report.notes.Count -eq 2) 'the report counts the unchanged files'
Assert ($report.zip.old -ne $report.zip.new -and $report.exe.old -and $report.exe.old -ne $report.exe.new) 'the report gives the old and new zip and exe'
Assert ($report.exe.signer -eq $publisher.thumbprint -and -not $report.update) 'the report names the signer, and no update assets for a package without them'

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
# BD-294: another signer than the old single exe's, and an old single exe without a signature.
$r = Invoke-Repack $pkgDir (Join-Path $base 'r-stranger') $newNotes $stranger
Assert ($r.code -ne 0 -and $r.text -match "signed by $($publisher.thumbprint) and the repack signer is $($stranger.thumbprint)") "a repack signer that did not sign the old exe: $($r.text)"
$unsignedDir = Join-Path $base 'package-unsigned\release'
$null = New-Package $unsignedDir -Secrets $null
$r = Invoke-Repack $unsignedDir (Join-Path $base 'r-unsigned') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'is not signed') "an old single exe without a signature: $($r.text)"
# A package folder that is not the zip that was gated.
[IO.File]::WriteAllText((Join-Path $oldTop 'files\kmd\bc250kmd.sys'), 'other driver bytes')
$r = Invoke-Repack $pkgDir (Join-Path $base 'r-tampered') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'not intact') 'a changed file of the old package'
Assert ($r.text -match 'kmd\\bc250kmd.sys does not match manifest.json') 'the problem names the file'

# BD-294: a release package with the self-update assets.
$updDir = Join-Path $base 'package-update\release'
$updTop = New-Package $updDir -Update -Pin $publisher.thumbprint
$oldUpdateText = [IO.File]::ReadAllText((Join-Path $updDir 'nagi-update.json'))
$r = Invoke-Repack $updDir (Join-Path $base 'repack-update') $newNotes
Assert ($r.code -eq 0) "the repack of a package with update assets passes: $($r.text)"
Assert ($r.text -match "signer $($publisher.thumbprint) is pinned") 'the repack checked the signature with the pinned list of the package'
$ru = Join-Path $base 'repack-update'
$newZip = Join-Path $ru "$name.zip"
$newUpdate = [IO.File]::ReadAllText((Join-Path $ru 'nagi-update.json')) | ConvertFrom-Json
$oldUpdate = $oldUpdateText | ConvertFrom-Json
$nd = @($newUpdate.components | Where-Object kind -eq 'driver')[0]
$od = @($oldUpdate.components | Where-Object kind -eq 'driver')[0]
Assert ($od.sha256 -eq (Get-Hash "$updTop.zip") -and $nd.sha256 -eq (Get-Hash $newZip) -and $nd.size -eq (Get-Item $newZip).Length) 'the new update manifest pins the new zip'
Assert ($nd.driver.package_manifest_sha256 -eq (Get-Hash (Join-Path $ru "$name\files\manifest.json"))) 'the new update manifest pins the new package manifest'
$nd.sha256 = $od.sha256; $nd.size = $od.size; $nd.driver.package_manifest_sha256 = $od.driver.package_manifest_sha256
Assert (($newUpdate | ConvertTo-Json -Depth 30 -Compress) -ceq ($oldUpdate | ConvertTo-Json -Depth 30 -Compress)) 'the update manifest changes in the three zip values only'
Assert ((Get-Hash (Join-Path $ru "nagi-apps-$version.zip")) -eq (Get-Hash (Join-Path $updDir "nagi-apps-$version.zip"))) 'the apps pack is the old one byte for byte'
Assert ((Get-Signer (Join-Path $ru "amdgpu-wddm-setup-$version.exe")) -eq $publisher.thumbprint) 'the single exe of the update package is signed'
# The installed release's own check, independent of the repack: Windows PowerShell 5.1, the fixture installer's pins.
$check = "& { . '$((Join-Path $ru "$name\files\installer\nagi-update.ps1").Replace("'", "''"))'; `$r = Test-NagiManifestSignature -ManifestPath '$((Join-Path $ru 'nagi-update.json').Replace("'", "''"))' -SignaturePath '$((Join-Path $ru 'nagi-update.json.p7s').Replace("'", "''"))'; if (`$r.ok) { 'OK ' + `$r.thumbprint; exit 0 }; `$r.reason; exit 1 }"
$text = & (Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $check 2>&1 | Out-String
Assert ($LASTEXITCODE -eq 0 -and $text -match "OK $($publisher.thumbprint)") "the new signature verifies in Windows PowerShell 5.1: $text"
$ur = Get-Content -LiteralPath (Join-Path $ru 'repack-report.json') -Raw | ConvertFrom-Json
Assert ($ur.update.manifest.old -ne $ur.update.manifest.new -and $ur.update.signature) 'the report gives the old and new update manifest'
# Refusals of the update step: a pinned list without the signer, and a release package that ships the self-update
# without its assets.
$pinDir = Join-Path $base 'package-pin\release'
$null = New-Package $pinDir -Update -Pin $stranger.thumbprint
$r = Invoke-Repack $pinDir (Join-Path $base 'r-pin') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'does not verify as the installed release checks it' -and $r.text -match 'not a pinned publisher') "a signer that the package does not pin: $($r.text)"
Remove-Item -LiteralPath (Join-Path $pinDir 'nagi-update.json')
$r = Invoke-Repack $pinDir (Join-Path $base 'r-noupdate') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'ships the self-update but .* has no nagi-update.json') "a release package without its update assets: $($r.text)"

# The lab flavour (BD-366): its folder, zip and single exe have the suffix -lab, and the repack keeps these names.
$labDir = Join-Path $base 'package\lab'
$null = New-Package $labDir -Flavour lab
$r = Invoke-Repack $labDir (Join-Path $base 'r-lab') $newNotes
Assert ($r.code -eq 0) "the repack of a lab package passes: $($r.text)"
foreach ($f in "$name-lab\files\manifest.json", "$name-lab.zip", "amdgpu-wddm-setup-$version-lab.exe") {
    Assert (Test-Path -LiteralPath (Join-Path $base "r-lab\$f")) "the lab repack has $f"
}
Assert ((Get-Hash (Join-Path $base "r-lab\$name-lab\files\RELEASE-NOTES.md")) -eq (Get-Hash (Join-Path $newNotes "$version.md"))) 'the lab repack has the new notes'
# A lab package in a folder without the suffix is refused.
$labBad = Join-Path $base 'package\lab-badname'
Copy-Item -LiteralPath $labDir -Destination $labBad -Recurse
Rename-Item -LiteralPath (Join-Path $labBad "$name-lab") $name
Rename-Item -LiteralPath (Join-Path $labBad "$name-lab.zip") "$name.zip"
$r = Invoke-Repack $labBad (Join-Path $base 'r-lab-badname') $newNotes
Assert ($r.code -ne 0 -and $r.text -match 'does not name the manifest version .* of the lab flavour') "a lab package folder without -lab: $($r.code) $($r.text)"

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
    $top = Join-Path $set "package\release\$name"
    Write-Manifest $files $top
    Remove-Item -LiteralPath "$top.zip", (Join-Path $set "package\release\amdgpu-wddm-setup-$version.exe")
    Complete-Package (Join-Path $set 'package\release') $top $publisher
    $text = (& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build-release-set.ps1') -RepackNotes -From $set -Flavours release `
        -SigningSecrets $publisher.dir 2>&1 | Out-String) -replace '\s+', ' '
    Assert ($LASTEXITCODE -eq 1 -and $text -match 'the flavour gate of the repacked release package fails') "-RepackNotes stops at the flavour gate: $text"
    $log = Get-Content -LiteralPath (Join-Path $set 'logs\repack-release\repack.log') -Raw
    Assert ($log -match 'repack-notes: ok') "the repack step of -RepackNotes passed: $log"
    Assert (Test-Path -LiteralPath (Join-Path $set "package-notes\release\$name.zip")) '-RepackNotes writes the new zip under package-notes'
    Assert ((Get-Signer (Join-Path $set "package-notes\release\amdgpu-wddm-setup-$version.exe")) -eq $publisher.thumbprint) '-RepackNotes signs the single exe with the -SigningSecrets certificate'
    $rr = Get-Content -LiteralPath (Join-Path $set 'repack-report.json') -Raw | ConvertFrom-Json
    Assert ($rr.status -eq 'failed' -and $rr.notes_commit) 'the report of -RepackNotes names the notes commit and the failure'
}

Remove-Item -LiteralPath $base -Recurse -Force
"PASS: notes-only repack, $passed checks (BD-265, BD-294)"
