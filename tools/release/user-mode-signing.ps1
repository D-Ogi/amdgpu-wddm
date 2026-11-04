# The signature of the user-mode files of a release (BD-160). Dot-sourced by build-release.ps1 and by
# test-user-mode-signing.ps1.
#
# Two kinds of file get two kinds of signature:
#   - the driver package (payload\kmd: the KMD, its catalog and every user-mode driver in the DriverStore layout).
#     With Secure Boot on, Windows 10 1607 and later loads a kernel driver only under a Microsoft root (the
#     Partner Center signature; a root that a user installs does not count), so that package goes to Microsoft
#     attestation. Until then the release puts the test certificate where the attestation signatures go
#     (mock-attestation.ps1). Nothing here signs a file of the driver package.
#   - every other PE file (the control app, the tools, the setup window), the single-file setup and the submission
#     CAB. These take the signer this file selects: the test certificate of secrets\release by default, or a
#     publisher's code-signing certificate from the Windows certificate store (an EV certificate, which Partner
#     Center also requires on the submission CAB), named by -UserModeSignerThumbprint, with an RFC 3161 time stamp.
#     Azure Trusted Signing signs user-mode files only, so it would fit this route as well and never the KMD.
#
# The functions take what they read from the system as script blocks, so that a host test can give them a store,
# signatures and files of its own.

$script:CodeSigningEku = '1.3.6.1.5.5.7.3.3'

# The certificate of a thumbprint in CurrentUser\My, then LocalMachine\My. Returns the certificate with a 'store'
# note property (CurrentUser or LocalMachine), or $null.
function Find-StoreCodeSigningCertificate([string]$Thumbprint) {
    foreach ($location in 'CurrentUser', 'LocalMachine') {
        $c = @(Get-ChildItem -LiteralPath "Cert:\$location\My" -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $Thumbprint })
        if ($c.Count) {
            $c[0] | Add-Member -NotePropertyName store -NotePropertyValue $location -Force
            return $c[0]
        }
    }
    return $null
}

# The signer of the user-mode files. No thumbprint: the test certificate (mode 'test'). A thumbprint: a code-signing
# certificate of the store with its private key, valid now, and a time stamp server (mode 'store'). Everything that
# is wrong with the request stops the build here, before anything is built.
function Get-UserModeSigner {
    param([string]$Thumbprint, [string]$TimestampUrl, [Parameter(Mandatory)][string]$Flavour,
        [string]$TestThumbprint, [scriptblock]$FindCertificate = { param($t) Find-StoreCodeSigningCertificate $t },
        [DateTime]$Now = [DateTime]::UtcNow)
    if (-not $Thumbprint) {
        if ($TimestampUrl) { throw '-TimestampUrl needs -UserModeSignerThumbprint: the test certificate is used without a time stamp' }
        return [pscustomobject]@{ mode = 'test'; thumbprint = $TestThumbprint; timestamp_url = $null; store = $null; subject = $null }
    }
    $Thumbprint = $Thumbprint.Trim().ToUpperInvariant()
    if ($Thumbprint -notmatch '^[0-9A-F]{40}$') { throw "-UserModeSignerThumbprint must be the 40 hexadecimal digits of a certificate thumbprint" }
    if ($Flavour -ne 'release') { throw 'a lab package stays test-signed: -UserModeSignerThumbprint is for the release flavour only' }
    if ($TestThumbprint -and $Thumbprint -eq $TestThumbprint.ToUpperInvariant()) { throw '-UserModeSignerThumbprint names the test certificate; leave it out to sign with that certificate' }
    if (-not $TimestampUrl) { throw '-UserModeSignerThumbprint needs -TimestampUrl: a publisher signature without a time stamp stops being valid when the certificate expires' }
    $uri = $null
    if (-not [Uri]::TryCreate($TimestampUrl, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -notin 'http', 'https') {
        throw "-TimestampUrl must be an http or https address: $TimestampUrl"
    }
    $cert = & $FindCertificate $Thumbprint
    if (-not $cert) { throw "no certificate $Thumbprint in CurrentUser\My or LocalMachine\My (for a cloud certificate, start its signing client first)" }
    if (-not $cert.HasPrivateKey) { throw "the certificate $Thumbprint has no private key on this computer" }
    $ekus = @($cert.EnhancedKeyUsageList | ForEach-Object { [string]$_.ObjectId })
    if ($ekus -notcontains $script:CodeSigningEku) { throw "the certificate $Thumbprint is not for code signing (no EKU $($script:CodeSigningEku))" }
    if ($cert.NotBefore.ToUniversalTime() -gt $Now -or $cert.NotAfter.ToUniversalTime() -le $Now) {
        throw "the certificate $Thumbprint is not valid now (from $($cert.NotBefore.ToString('u')) to $($cert.NotAfter.ToString('u')))"
    }
    [pscustomobject]@{ mode = 'store'; thumbprint = $Thumbprint; timestamp_url = $uri.AbsoluteUri
        store = [string]$cert.store; subject = [string]$cert.Subject }
}

# The signtool arguments for one file. The test route names the PFX and its password; the store route names the
# certificate by thumbprint and its store, and asks for an RFC 3161 time stamp with SHA-256.
function Get-SignToolSignArguments {
    param([Parameter(Mandatory)]$Signer, [Parameter(Mandatory)][string]$Path, [string]$Pfx, [string]$Password)
    if ($Signer.mode -eq 'test') {
        if (-not $Pfx -or -not $Password) { throw 'the test route needs the PFX and its password' }
        return @('sign', '/fd', 'SHA256', '/f', $Pfx, '/p', $Password, $Path)
    }
    if ($Signer.mode -ne 'store') { throw "unknown signer mode $($Signer.mode)" }
    $a = @('sign', '/fd', 'SHA256', '/sha1', $Signer.thumbprint, '/s', 'My')
    if ($Signer.store -eq 'LocalMachine') { $a += '/sm' }
    $a + @('/tr', $Signer.timestamp_url, '/td', 'SHA256', $Path)
}

# The signtool arguments for the detached signature of the update manifest (docs/design/self-update.md): the same
# signer as every user-mode file, with /p7 so that signtool writes <OutDir>\<file name>.p7, a CMS SignedData over the
# file's bytes (content type data, detached), and leaves the file itself as it is. The time stamp arguments go: the
# installed release checks the signature itself and the pinned signer, not the certificate's validity period.
function Get-ManifestSignToolArguments {
    param([Parameter(Mandatory)]$Signer, [Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$OutDir, [string]$Pfx, [string]$Password)
    $a = @(Get-SignToolSignArguments -Signer $Signer -Path $Path -Pfx $Pfx -Password $Password)
    $out = New-Object Collections.Generic.List[string]
    for ($i = 0; $i -lt $a.Count - 1; $i++) {
        if ($a[$i] -in '/tr', '/td') { $i++; continue }
        $out.Add($a[$i])
    }
    $out.AddRange([string[]]@('/p7', $OutDir, '/p7co', '1.2.840.113549.1.7.1', '/p7ce', 'DetachedSignedData', $Path))
    return $out.ToArray()
}

# signtool /p7 writes the bare SignedData structure (SEQUENCE, version 1, ...). A CMS reader (SignedCms.Decode in
# Windows PowerShell 5.1 and in the control application) wants it inside a ContentInfo: SEQUENCE { signedData OID,
# [0] EXPLICIT SignedData }. The release ships that standard form as nagi-update.json.p7s.
function ConvertTo-CmsContentInfo([byte[]]$SignedData) {
    function DerLength([int]$n) {
        if ($n -lt 0x80) { return , [byte[]]@($n) }
        $b = New-Object Collections.Generic.List[byte]
        while ($n -gt 0) { $b.Insert(0, [byte]($n -band 0xFF)); $n = $n -shr 8 }
        return , [byte[]](@([byte](0x80 -bor $b.Count)) + $b.ToArray())
    }
    if (-not $SignedData -or $SignedData.Length -lt 4 -or $SignedData[0] -ne 0x30) { throw 'the signtool output is not a DER SEQUENCE' }
    $oid = [byte[]](0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02)
    $inner = [byte[]](@([byte]0xA0) + (DerLength $SignedData.Length) + $SignedData)
    $body = [byte[]]($oid + $inner)
    return , [byte[]](@([byte]0x30) + (DerLength $body.Length) + $body)
}

# Checks the signature of a file the route signed, and throws when it is not the signer's. The store route needs a
# signature that Windows trusts (Valid), the signer's thumbprint and a time stamp. The test route needs the test
# certificate's thumbprint and a signature whose hash matches; its root is trusted only where the installer put it,
# so Valid is not required here (the single-file setup gets the pinned check of driver-signature.ps1 as well).
function Test-UserModeSignature {
    param([Parameter(Mandatory)]$Signer, [Parameter(Mandatory)][string]$Path,
        [scriptblock]$ReadSignature = { param($p) Get-AuthenticodeSignature -LiteralPath $p })
    $s = & $ReadSignature $Path
    $status = [string]$s.Status
    $thumb = $(if ($s.SignerCertificate) { [string]$s.SignerCertificate.Thumbprint } else { '' })
    if ($thumb -ne $Signer.thumbprint) { throw "$Path is not signed by $($Signer.thumbprint) (signer '$thumb', status $status)" }
    if ($Signer.mode -eq 'store') {
        if ($status -ne 'Valid') { throw "the signature of $Path is not valid on this computer (status $status)" }
        if (-not $s.TimeStamperCertificate) { throw "the signature of $Path has no time stamp" }
    } elseif ($status -in 'NotSigned', 'HashMismatch', 'NotSupportedFileFormat', 'Incompatible') {
        throw "the signature of $Path is not usable (status $status)"
    }
}

# The PE files under a package root that carry no signature yet, split into the files of the driver package and
# the others. A file that is already signed keeps its signature (a program of another publisher, or a driver file
# that mock attestation signed).
function Split-UnsignedPeFiles {
    param([Parameter(Mandatory)][string]$PackageRoot, [Parameter(Mandatory)][string]$DriverDirectory,
        [scriptblock]$IsSigned = { param($p) [bool](Get-AuthenticodeSignature -LiteralPath $p).SignerCertificate })
    $driverRoot = [IO.Path]::GetFullPath($DriverDirectory).TrimEnd('\') + '\'
    $driver = New-Object Collections.Generic.List[string]
    $user = New-Object Collections.Generic.List[string]
    foreach ($f in @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | Where-Object { $_.Extension -in '.dll', '.exe', '.sys' } | Sort-Object FullName)) {
        if (& $IsSigned $f.FullName) { continue }
        if ($f.FullName.StartsWith($driverRoot, [StringComparison]::OrdinalIgnoreCase)) { $driver.Add($f.FullName) } else { $user.Add($f.FullName) }
    }
    [pscustomobject]@{ driver = @($driver); user = @($user) }
}
