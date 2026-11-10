# The build-to-package identity gate (BD-342). The registered release-sources hashes belong to the build artifacts;
# the package carries those artifacts signed, so a signed file's SHA-256 differs from the registered hash while its
# image identity (the Windows catalog hash, WDK mscat.h) stays the same. This gate checks that link for every payload
# file of the package:
#   - a PE file (.sys, .dll, .exe): its image identity equals the image identity of the registered artifact, and its
#     signer is one of the pinned certificates; a third_party row is the vendor's file unchanged: the whole SHA-256
#     equals the registered one, the image hash equals the source's when the file is a PE, and the package signer is
#     the source's own signer (no pinned signer);
#   - every other file: byte-equal to the registered artifact (same SHA-256);
#   - a signature whose status is HashMismatch is refused (its blob does not match its image), and Status Valid is not
#     required: an unimported test chain reads UnknownError, and chain trust is not this gate's scope;
#   - with -DriverMembers, every .sys/.dll/.exe of the package tree must be a source row or a driver package member:
#     a PE that lands in files\ without a row is refused.
# A driver package row's registered path is relative to the driver package root (payload/d3d11/...), while its file
# sits under the package at payload/kmd/d3d11/...; -DriverMembers (the driver_package.members records, with their
# source and path) makes that translation, and without it the gate would look for the file in the wrong place.
# The image hash comes from image_identity.py, the same code that mock-attestation.ps1 uses for the driver payload;
# this file does not implement a second hash.
#   Test-PackageIdentity -PackageRoot <files\> -Sources @($sources.files) -SignerThumbprints @(<release>,<user mode>)
#     [-DriverMembers $driverPackage.members] [-Python <python.exe>] [-Root <workspace root>]
# Prints one REFUSE line per file that fails (to the host), then throws; otherwise prints the PASS line.
function Get-PeImageHash([string]$Path, [string]$Python) {
    if (-not $Python) { $Python = (Get-Command python -ErrorAction SilentlyContinue).Source }
    if (-not $Python) { throw 'no python to compute the image identity' }
    $identity = Join-Path $PSScriptRoot 'image_identity.py'
    $text = (& $Python -I $identity --file $Path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "image identity $Path failed: $text" }
    $hash = ($text -split '\s+' | Where-Object { $_ -match '^[0-9A-Fa-f]{64}$' } | Select-Object -First 1)
    if (-not $hash) { throw "image identity $Path printed no hash: $text" }
    return $hash.ToUpperInvariant()
}

function Get-Signature([string]$Path) {
    $s = Get-AuthenticodeSignature -LiteralPath $Path
    return [pscustomobject]@{
        thumbprint = $(if ($s.SignerCertificate) { $s.SignerCertificate.Thumbprint.ToUpperInvariant() } else { '' })
        status = [string]$s.Status
    }
}

function Test-PackageIdentity {
    param([Parameter(Mandatory)][string]$PackageRoot, [Parameter(Mandatory)][object[]]$Sources,
          [string[]]$SignerThumbprints = @(), [object[]]$DriverMembers = @(), [string[]]$EditedPaths = @(), [string]$Python = '', [string]$Root = '')
    if (-not $Root) { $Root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) } }
    if (-not $SignerThumbprints.Count) { throw 'Test-PackageIdentity needs at least one pinned signer thumbprint' }
    $pinned = @($SignerThumbprints | ForEach-Object { $_.ToUpperInvariant() })
    $refused = [Collections.Generic.List[string]]::new()
    # The driver package rows name the file by the driver package root; the member record names the package path.
    $memberMap = @{}; $memberPaths = @{}
    foreach ($m in $DriverMembers) {
        $ms = ([string]$m.source) -replace '/', '\'
        $mp = ([string]$m.path) -replace '/', '\'
        if ($ms) { $memberMap[$ms] = $mp }
        if ($mp) { $memberPaths[$mp] = $true }
    }
    $edited = @{}
    foreach ($e in $EditedPaths) { $edited[(([string]$e) -replace '/', '\')] = $true }
    $packagePaths = @{}
    $pe = 0; $data = 0; $tpCount = 0
    foreach ($f in $Sources) {
        $regRel = ([string]$f.path) -replace '/', '\'
        $rel = if ($memberMap.ContainsKey($regRel)) { $memberMap[$regRel] } else { $regRel }
        $packagePaths[$rel] = $true
        $pkgFile = Join-Path $PackageRoot $rel
        $srcFile = Join-Path $Root (([string]$f.source) -replace '/', '\')
        if (-not (Test-Path -LiteralPath $pkgFile)) { $refused.Add("${rel}: missing in the package"); continue }
        if (-not (Test-Path -LiteralPath $srcFile)) { $refused.Add("${rel}: registered source is missing: $($f.source)"); continue }
        $isPe = $pkgFile -match '\.(sys|dll|exe)$'
        $third = [bool]$f.third_party
        if ($third) {
            $tpCount++
            $got = (Get-FileHash -LiteralPath $pkgFile -Algorithm SHA256).Hash.ToUpperInvariant()
            if ($got -ne ([string]$f.sha256).ToUpperInvariant()) {
                $refused.Add("${rel}: a third-party file must be byte-equal to the registered $([string]$f.sha256) (it is $got)")
                continue
            }
            if ($isPe) {
                $want = Get-PeImageHash $srcFile $Python
                $gotImg = Get-PeImageHash $pkgFile $Python
                if ($gotImg -ne $want) { $refused.Add("${rel}: image $gotImg is not the source image ${want}"); continue }
            }
            $pkgSig = Get-Signature $pkgFile
            if ($pkgSig.status -eq 'HashMismatch') { $refused.Add("${rel}: the signature does not match the image (HashMismatch)"); continue }
            $srcSig = Get-Signature $srcFile
            if ($pkgSig.thumbprint -ne $srcSig.thumbprint) {
                $refused.Add("${rel}: signed by $($pkgSig.thumbprint), the source by $($srcSig.thumbprint): a third-party file keeps its vendor signature")
            }
            continue
        }
        if ($isPe) {
            $pe++
            $want = Get-PeImageHash $srcFile $Python
            $got = Get-PeImageHash $pkgFile $Python
            if ($got -ne $want) {
                $refused.Add("${rel}: image $got is not the built image ${want} (whole file $((Get-FileHash -LiteralPath $pkgFile -Algorithm SHA256).Hash), registered $([string]$f.sha256))")
                continue
            }
            $sig = Get-Signature $pkgFile
            if (-not $sig.thumbprint) { $refused.Add("${rel}: image $got is right, but the file is unsigned"); continue }
            if ($sig.status -eq 'HashMismatch') { $refused.Add("${rel}: image $got is right, but the signature does not match the image (HashMismatch)"); continue }
            if ($pinned -notcontains $sig.thumbprint) { $refused.Add("${rel}: image $got is right, but it is signed by $($sig.thumbprint) and not by a pinned signer ($($pinned -join ','))") }
        }
        else {
            $data++
            $got = (Get-FileHash -LiteralPath $pkgFile -Algorithm SHA256).Hash.ToUpperInvariant()
            if ($got -ne ([string]$f.sha256).ToUpperInvariant()) {
                if ($edited.ContainsKey($rel) -or [string]$f.release_edits) { "note ${rel}: a declared packaging edit (bytes differ from the source by design)" }
                else { $refused.Add("${rel}: data file $got is not the registered $([string]$f.sha256) and no packaging edit is declared for it") }
            }
        }
    }
    if ($DriverMembers.Count) {
        foreach ($f in @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.sys', '.dll', '.exe' })) {
            $rel = $f.FullName.Substring($PackageRoot.Length + 1)
            if (-not $packagePaths.ContainsKey($rel) -and -not $memberPaths.ContainsKey($rel)) {
                $refused.Add("${rel}: a PE of the package that no source row or driver package member names")
            }
        }
    }
    if ($refused.Count) {
        $refused | ForEach-Object { Write-Host "REFUSE package identity: $_" }
        throw "$($refused.Count) file(s) of the package do not carry the registered image under a pinned signer"
    }
    "PASS package identity: $($Sources.Count) file(s), $pe PE by image hash, $tpCount third-party, $data by SHA-256, signer $($pinned -join ',')"
}
