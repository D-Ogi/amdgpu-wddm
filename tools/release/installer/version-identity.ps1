# BD-152: installation owns the Windows driver version. Pure, shared by the build,
# installer and host tests; PowerShell 5.1. No registry or package mutation here.

function ConvertTo-DriverQuad([string]$Text) {
    if ($Text -cnotmatch '\A(?:0|[1-9][0-9]{0,4})(?:\.(?:0|[1-9][0-9]{0,4})){3}\z') {
        throw "not a canonical driver version quad: '$Text'"
    }
    $parts = @($Text.Split('.') | ForEach-Object { [int]$_ })
    if (@($parts | Where-Object { $_ -gt 65534 }).Count) { throw "driver version field exceeds 65534: '$Text'" }
    return [version]$Text
}

function Get-ProductPackageQuad([string]$Release) {
    if ($Release -cnotmatch '\A([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?\z') {
        throw "invalid product release version: '$Release'"
    }
    return (ConvertTo-DriverQuad $Matches[1]).ToString()
}

function Get-PackageDriverQuad([string]$ProductQuad) {
    $v = ConvertTo-DriverQuad $ProductQuad
    if ($v.Major -gt 65494) { throw "product major exceeds the supported +40 mapping: '$ProductQuad'" }
    return '{0}.{1}.{2}.{3}' -f ($v.Major + 40), $v.Minor, $v.Build, $v.Revision
}

function ConvertTo-DriverDate([string]$Text) {
    $date = [DateTime]::MinValue
    if (-not [DateTime]::TryParseExact($Text, 'MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$date)) { throw "invalid INF DriverVer date: '$Text'" }
    return $date
}

function Get-InfDriverIdentity([string]$Text) {
    $section = ''; $entries = @()
    foreach ($line in ($Text -split "`r?`n")) {
        $line = ($line -replace ';.*$', '').Trim()
        if ($line -match '^\[([^\]]+)\]$') { $section = $Matches[1]; continue }
        if ($section -ieq 'Version' -and $line -match '^DriverVer\s*=\s*(.*)$') { $entries += $Matches[1] }
    }
    if ($entries.Count -ne 1 -or $entries[0] -notmatch '\A([0-9]{2}/[0-9]{2}/[0-9]{4}),\s*([0-9.]+)\z') {
        throw 'INF must have exactly one valid DriverVer in [Version]'
    }
    $dateText = $Matches[1]; $quad = (ConvertTo-DriverQuad $Matches[2]).ToString()
    [void](ConvertTo-DriverDate $dateText)
    return [pscustomobject]@{ date = $dateText; quad = $quad }
}

function New-PackageVersionIdentity {
    param([string]$Release, [string]$DriverDate, [string]$KmdBuild, [string]$KmdAbi, [string]$KmdSha256)
    $product = Get-ProductPackageQuad $Release
    [void](ConvertTo-DriverDate $DriverDate)
    [void](ConvertTo-DriverQuad $KmdBuild)
    if ($KmdAbi -cnotmatch '\A0x[0-9a-fA-F]{8}\z' -or $KmdSha256 -notmatch '\A[0-9a-fA-F]{64}\z') {
        throw 'invalid embedded KMD ABI or image hash'
    }
    return [ordered]@{
        schema = 1; release = $Release; product_quad = $product
        inf = [ordered]@{ date = $DriverDate; quad = (Get-PackageDriverQuad $product) }
        kmd = [ordered]@{ build = $KmdBuild; abi = $KmdAbi; sha256 = $KmdSha256.ToUpperInvariant() }
    }
}

# A malformed new identity never falls through to the legacy alias. kmd_version
# historically meant the INF quad, not the embedded SYS version; retain that meaning
# for old manifests and as a checked compatibility alias in new ones.
function Get-ManifestDriverQuad($Manifest) {
    if (-not $Manifest) { throw 'no package manifest' }
    $hasIdentity = if ($Manifest -is [Collections.IDictionary]) { $Manifest.Contains('version_identity') }
        else { $null -ne $Manifest.PSObject.Properties['version_identity'] }
    if ($hasIdentity) {
        $i = $Manifest.version_identity
        if (-not $i -or $i.schema -ne 1 -or $i.release -cne [string]$Manifest.version) { throw 'invalid package version identity' }
        $product = Get-ProductPackageQuad ([string]$Manifest.version)
        $mapped = Get-PackageDriverQuad $product
        [void](ConvertTo-DriverDate ([string]$i.inf.date))
        if ($i.product_quad -cne $product -or $i.inf.quad -cne $mapped -or
            $i.kmd.build -cne [string]$Manifest.kmd_build -or $i.kmd.abi -cne [string]$Manifest.kmd_abi -or
            [string]$i.kmd.abi -cnotmatch '\A0x[0-9a-fA-F]{8}\z' -or
            [string]$i.kmd.sha256 -notmatch '\A[0-9a-fA-F]{64}\z' -or
            [string]$Manifest.kmd_version -cne $mapped) { throw 'package version identity fields disagree' }
        [void](ConvertTo-DriverQuad ([string]$i.kmd.build))
        return $mapped
    }
    return (ConvertTo-DriverQuad ([string]$Manifest.kmd_version)).ToString()
}

function Test-PackageVersionIdentity($Manifest, [string]$InfText, [string]$KmdSha256) {
    $quad = Get-ManifestDriverQuad $Manifest
    $inf = Get-InfDriverIdentity $InfText
    if ($inf.quad -cne $quad) { throw 'bound package INF version differs from manifest' }
    if ($Manifest.version_identity) {
        $i = $Manifest.version_identity
        if ($inf.date -cne $i.inf.date -or $i.kmd.sha256 -ine $KmdSha256) { throw 'package date or KMD image differs from version identity' }
        $productRows = @([regex]::Matches($InfText, '(?im)^HKR\s*,\s*,\s*ProductPackageVersion\s*,\s*0x00000000\s*,\s*"([0-9.]+)"\s*$'))
        if ($productRows.Count -ne 1 -or $productRows[0].Groups[1].Value -cne $i.product_quad) { throw 'INF product identity is absent or disagrees' }
    }
    return $inf
}

# BD-161: a legacy KMD may still be loaded at the first post-upgrade check, or
# leave its alias until the next start after the option was disabled. The pair
# of private records must prove both sides. Arithmetic on the major is no proof.
function Test-InstalledDriverVersion {
    param([Alias('Reading')][string]$Installed,[Alias('Want')][string]$Expected,[Alias('Backup')][string]$Original,
        [string]$Reported,[bool]$InstallationOwned=$false)
    $result=[pscustomobject]@{ok=$false;note=''}
    if(-not $Installed -or -not $Expected){return $result}
    try { [void](ConvertTo-DriverQuad $Installed);[void](ConvertTo-DriverQuad $Expected) } catch { return $result }
    if($Installed -ceq $Expected){$result.ok=$true;return $result}
    if(-not $InstallationOwned -and $Original -ceq $Expected -and $Reported -ceq $Installed) {
        $result.ok=$true;$result.note="; legacy AMD numbering is proved by private records, installed $Original"
    }
    return $result
}
