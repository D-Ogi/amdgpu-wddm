# BD-152: one immutable Driver Store firmware set. Dot-sourcing does no I/O.
# Private staging and catalog membership execute on Windows PowerShell 5.1.

function Get-DriverStoreFirmwareNames {
    return @('ce', 'me', 'mec', 'mec2', 'pfp', 'rlc', 'sdma', 'sdma1' | ForEach-Object { "cyan_skillfish2_$_.bin" })
}

function Test-DriverStoreFirmware($Firmware) {
    $hasDelivery = $null -ne $Firmware -and ($null -ne $Firmware.PSObject.Properties['delivery'] -or
        ($Firmware -is [Collections.IDictionary] -and $Firmware.Contains('delivery')))
    if (-not $hasDelivery) {
        if ($Firmware.install_dir -ceq '%13%') { throw 'Driver Store firmware delivery contract is missing' }
        return $false # Old repair sets retain their own delivery contract.
    }
    if ($Firmware.delivery -cne 'driver-store-v1' -or $Firmware.install_dir -cne '%13%' -or
        [string]$Firmware.commit -cnotmatch '\A[0-9a-f]{40}\z') { throw 'invalid Driver Store firmware delivery' }
    $names = @(Get-DriverStoreFirmwareNames) + @('LICENSE.amdgpu')
    $rows = @($Firmware.files)
    if ($rows.Count -ne $names.Count) { throw 'Driver Store firmware set must contain eight binaries and the licence' }
    foreach ($name in $names) {
        $f = @($rows | Where-Object { $_.name -ceq $name })
        if ($f.Count -ne 1 -or $f[0].sha256 -isnot [string] -or $f[0].sha256 -notmatch '\A[0-9a-fA-F]{64}\z' -or
            ($f[0].size -isnot [int] -and $f[0].size -isnot [long]) -or $f[0].size -le 0 -or $f[0].size -gt 1048576) {
            throw "invalid pinned firmware member $name"
        }
    }
    return $true
}

# Used before catalog generation and before install. Nothing is fetched here.
function Copy-DriverStoreFirmware {
    param($Firmware, [string]$FromDir, [string]$Destination)
    if (-not (Test-DriverStoreFirmware $Firmware)) { throw 'package has no Driver Store firmware contract' }
    foreach ($name in Get-DriverStoreFirmwareNames) {
        $f = @($Firmware.files | Where-Object { $_.name -ceq $name })[0]
        $src = Join-Path $FromDir $name
        if (-not [IO.File]::Exists($src) -or (Get-Item -LiteralPath $src).Length -ne $f.size -or
            (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash -ine $f.sha256) { throw "firmware input differs from pin: $name" }
    }
    [void][IO.Directory]::CreateDirectory($Destination)
    foreach ($name in Get-DriverStoreFirmwareNames) {
        [IO.File]::Copy((Join-Path $FromDir $name), (Join-Path $Destination $name), $true)
        $f = @($Firmware.files | Where-Object { $_.name -ceq $name })[0]
        $dst = Join-Path $Destination $name
        if ((Get-Item -LiteralPath $dst).Length -ne $f.size -or (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -ine $f.sha256) {
            throw "copied firmware differs from pin: $name"
        }
    }
}

function Initialize-FirmwarePackageNative {
    if (-not ('AmdgpuWddm.PackageStage' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'PackageStage.cs')
    }
}

function Test-DriverCatalogMembers {
    param([string]$Directory, [string[]]$Names)
    Initialize-FirmwarePackageNative
    $cat = Join-Path $Directory 'bc250kmd.cat'
    foreach ($name in $Names) {
        if ($name -cne [IO.Path]::GetFileName($name) -or $name.Contains(':') -or $name.Contains('/')) { throw 'invalid catalog member basename' }
        if (-not [AmdgpuWddm.CatalogMembership]::Contains($cat, (Join-Path $Directory $name))) {
            throw "signed catalog does not contain these file bytes: $name"
        }
    }
}

# Return a scope which holds files against write/delete through the native stage
# call. The caller must dispose it, including on refusal. No receipt on failure.
function New-HydratedDriverPackage {
    param($Manifest, [string]$PackageRoot, [string]$FirmwareDir, [string]$Parent)
    if (-not (Test-DriverStoreFirmware $Manifest.firmware)) { throw 'package has no Driver Store firmware contract' }
    $bad = Test-FirmwareFolder $Manifest.firmware $FirmwareDir
    if ($bad.Count) { throw "firmware not ready: $($bad -join '; ')" }
    $inputs = @($Manifest.files | Where-Object { $_.path -clike 'payload/kmd/*' })
    $names = @($inputs | ForEach-Object { ([string]$_.path).Substring(12) })
    $infText=[IO.File]::ReadAllText((Join-Path $PackageRoot 'payload\kmd\bc250kmd.inf'))
    Assert-DriverStatePackageMembers -Names $names -InfText $infText
    Initialize-FirmwarePackageNative
    $scope = New-Object AmdgpuWddm.PackageStage $Parent
    try {
        foreach ($f in $inputs) {
            $name = ([string]$f.path).Substring(12)
            $scope.Copy((Join-Path $PackageRoot $f.path), $name, [long]$f.size)
        }
        foreach ($name in Get-DriverStoreFirmwareNames) {
            $f = @($Manifest.firmware.files | Where-Object { $_.name -ceq $name })[0]
            $scope.Copy((Join-Path $FirmwareDir $name), $name, [long]$f.size)
        }
        $scope.Seal()
        foreach ($f in $inputs) {
            $p = Join-Path $scope.DirectoryPath (([string]$f.path).Substring(12))
            if ((Get-Sha256 $p) -ine $f.sha256 -or (Get-Item -LiteralPath $p).Length -ne $f.size) {
                throw "private staged file differs from manifest: $($f.path)"
            }
        }
        foreach ($f in $Manifest.firmware.files) {
            if ($f.name -ceq 'LICENSE.amdgpu') { continue }
            $p = Join-Path $scope.DirectoryPath $f.name
            if ((Get-Sha256 $p) -ine $f.sha256 -or (Get-Item -LiteralPath $p).Length -ne $f.size) { throw "private firmware differs from pin: $($f.name)" }
        }
        $members = @($names | Where-Object { $_ -cne 'bc250kmd.cat' -and $_ -cnotmatch '\.cer\z' }) + @(Get-DriverStoreFirmwareNames)
        Test-DriverCatalogMembers $scope.DirectoryPath $members
        return $scope
    } catch { $scope.Dispose(); throw }
}

function Assert-DriverStatePackageMembers {
    param([string[]]$Names,[string]$InfText)
    $required=@('bc250kmd.inf','bc250kmd.sys','bc250kmd.cat')
    if($InfText -match '(?im)^\s*HKR\s*,\s*,\s*AmdgpuStateCatalog\s*,'){$required+='amdgpu-state.bin'}
    if($Names.Count -ne $required.Count){throw 'invalid manifest KMD payload set'}
    foreach($name in $required) {
        if(@($Names | Where-Object {$_ -ceq $name}).Count -ne 1){throw "missing or duplicate KMD payload: $name"}
    }
}

function Assert-DriverCatalogSigner {
    param([string]$Directory, [string]$Certificate)
    $expected = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $Certificate
    try {
        $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $Directory 'bc250kmd.cat')
        if ([string]$sig.Status -cne 'Valid' -or -not $sig.SignerCertificate -or
            $sig.SignerCertificate.Thumbprint -ine $expected.Thumbprint) { throw 'driver catalog signature is not trusted with the package certificate' }
    } finally { $expected.Dispose() }
}
