#Requires -Version 7.0
# Host test of package-identity.ps1 (BD-342): the build-to-package identity gate. A small "build" holds one PE file and
# one data file; a small "package" holds the PE signed by the run's publisher certificate and a copy of the data file.
# Cases: the positive package passes; a PE whose image differs from the build is refused; a PE signed by a stranger is
# refused although its image is right; the negative controls (a changed data file, an unsigned PE) are refused.
# A code-signing certificate of this run signs the files (no certificate store is opened); its PFX, password file and
# certificate lie under -Out for the run and are deleted at the end. The PE is a copy of an unsigned toolchain tool
# (llvm-mc.exe). A System32 program is not usable: hostname.exe carries a Microsoft signature, and an image that is
# in the system catalog is verified against that catalog, so Windows reports the Microsoft signer whatever the
# file embeds.
#   pwsh -File tools\release\test-package-identity-gate.ps1 -Out <dir> [-SignTool <signtool.exe>]
param([Parameter(Mandatory)][string]$Out, [string]$SignTool)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'user-mode-signing.ps1')
. (Join-Path $PSScriptRoot 'package-identity.ps1')
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('package-identity-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$passed = 0
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++; "ok $What" }
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Write-Bytes([string]$Path, [byte[]]$Bytes) { New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null; [IO.File]::WriteAllBytes($Path, $Bytes) }
if (-not $python) { throw 'no python on this host (image_identity.py needs it)' }
if (-not $SignTool -and $env:BC250_ROOT) { $SignTool = Join-Path $env:BC250_ROOT 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe' }
if (-not $SignTool) { $SignTool = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot)))) 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe' }
if (-not (Test-Path -LiteralPath $SignTool -PathType Leaf)) { throw "no signtool at ${SignTool}: pass -SignTool or set BC250_ROOT" }
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Split-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot)))) }
$stub = Join-Path $root 'toolchain\llvm-23.1.2-win64\bin\llvm-mc.exe'
if (-not (Test-Path -LiteralPath $stub)) { throw "no unsigned test PE at ${stub}: set BC250_ROOT" }

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
        pass = (Join-Path $Dir 'amdgpu-wddm-release.pass') }
}
function Invoke-RunSign([string]$Path, $Secrets) {
    $signer = [pscustomobject]@{ mode = 'test'; thumbprint = $Secrets.thumbprint; timestamp_url = $null; store = $null; subject = $null }
    $a = Get-SignToolSignArguments -Signer $signer -Path $Path -Pfx $Secrets.pfx -Password (Get-Content -LiteralPath $Secrets.pass -Raw)
    $text = (& $SignTool @a 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "signtool sign $Path failed: $text" }
}

$publisher = New-SecretsDir (Join-Path $base 'secrets') (New-RunCert 'Nagi identity test publisher')
$stranger = New-SecretsDir (Join-Path $base 'secrets-stranger') (New-RunCert 'Nagi identity test stranger')
$build = Join-Path $base 'build'
$pkg = Join-Path $base 'package'
Copy-Item -LiteralPath $stub -Destination (Join-Path $base 'src.exe')
New-Item -ItemType Directory -Force (Join-Path $build 'payload'), (Join-Path $pkg 'payload') | Out-Null
Copy-Item -LiteralPath (Join-Path $base 'src.exe') -Destination (Join-Path $build 'payload\app.exe')
Write-Bytes (Join-Path $build 'payload\data.bin') ([byte[]](1..64))
$sources = @(
    [pscustomobject]@{ path = 'payload/app.exe'; source = 'build/payload/app.exe'; sha256 = (Get-Hash (Join-Path $build 'payload\app.exe')) },
    [pscustomobject]@{ path = 'payload/data.bin'; source = 'build/payload/data.bin'; sha256 = (Get-Hash (Join-Path $build 'payload\data.bin')) })
$pinned = @($publisher.thumbprint)
# The gate writes its REFUSE lines to the host and then throws; capture both streams of one call.
function Invoke-Gate([object[]]$Src = $script:sources, [string]$Pkg = $script:pkg, [string[]]$Members = @()) {
    $sb = {
        param($Root, $Pkg, $Sources, $Pinned, $Python, $Members)
        try { $null = Test-PackageIdentity -PackageRoot $Pkg -Sources $Sources -SignerThumbprints $Pinned -DriverMembers $Members -Python $Python -Root $Root }
        catch { "THREW $($_.Exception.Message)" }
    }
    return @(& $sb $base $Pkg $Src $pinned $python $Members 6>&1) -join "`n"
}


# 1. The positive package: the image signed by the pinned signer, the data file unchanged.
Copy-Item -LiteralPath (Join-Path $build 'payload\app.exe') -Destination (Join-Path $pkg 'payload\app.exe')
Copy-Item -LiteralPath (Join-Path $build 'payload\data.bin') -Destination (Join-Path $pkg 'payload\data.bin')
Invoke-RunSign (Join-Path $pkg 'payload\app.exe') $publisher
$line = Test-PackageIdentity -PackageRoot $pkg -Sources $sources -SignerThumbprints $pinned -Python $python -Root $base
Assert ($line -match '^PASS package identity: 2 file\(s\), 1 PE by image hash,') "the positive package passes: $line"
Assert ((Get-Hash (Join-Path $pkg 'payload\app.exe')) -ne (Get-Hash (Join-Path $build 'payload\app.exe'))) 'the signed file has another whole-file hash than the build artifact'

# 2. A PE whose image differs from the build: one byte of the image changed, then signed.
$bytes = [IO.File]::ReadAllBytes((Join-Path $build 'payload\app.exe'))
$edit = [byte[]]$bytes.Clone()
$edited = $false
for ($i = 4096; $i -lt $edit.Length - 512; $i++) { if ($edit[$i] -ne 0) { $edit[$i] = [byte](($edit[$i] + 1) % 256); $edited = $true; break } }
Assert $edited 'a non-zero byte of the image was found to change'
Write-Bytes (Join-Path $pkg 'payload\app.exe') $edit
Invoke-RunSign (Join-Path $pkg 'payload\app.exe') $publisher
$out = Invoke-Gate
Assert ($out -match 'is not the built image' -and $out -match 'THREW') 'a changed image is refused'

# 3. A PE with the right image but another signer.
Copy-Item -LiteralPath (Join-Path $build 'payload\app.exe') -Destination (Join-Path $pkg 'payload\app.exe')
Invoke-RunSign (Join-Path $pkg 'payload\app.exe') $stranger
$out = Invoke-Gate
Assert ($out -match 'signed by ' -and $out -match 'THREW') 'a stranger signer is refused'

# 4. The negative controls: a changed data file, and a PE that is not signed.
Copy-Item -LiteralPath (Join-Path $build 'payload\app.exe') -Destination (Join-Path $pkg 'payload\app.exe')
Invoke-RunSign (Join-Path $pkg 'payload\app.exe') $publisher
$d = [IO.File]::ReadAllBytes((Join-Path $pkg 'payload\data.bin')); $d[3] = [byte](($d[3] + 1) % 256); Write-Bytes (Join-Path $pkg 'payload\data.bin') $d
$out = Invoke-Gate
Assert ($out -match 'REFUSE package identity: payload\\data\.bin: data file' -and $out -match 'THREW') 'a changed data file is refused (negative control)'
Copy-Item -LiteralPath (Join-Path $build 'payload\data.bin') -Destination (Join-Path $pkg 'payload\data.bin')
Copy-Item -LiteralPath (Join-Path $build 'payload\app.exe') -Destination (Join-Path $pkg 'payload\app.exe')
$out = Invoke-Gate
Assert ($out -match 'unsigned' -and $out -match 'THREW') 'an unsigned PE is refused (negative control)'

# 5. A third-party row: the vendor's own file carried unchanged (its own signer, no pin).
$tpsrc = Join-Path $base 'vendor'
$tpPkg = Join-Path $base 'package-vendor'
New-Item -ItemType Directory -Force (Join-Path $tpsrc 'payload'), (Join-Path $tpPkg 'payload') | Out-Null
$vendor = Join-Path $base 'vendor-app.exe'
Copy-Item -LiteralPath $stub -Destination $vendor -Force
Invoke-RunSign $vendor $stranger
Copy-Item -LiteralPath $vendor -Destination (Join-Path $tpsrc 'payload\vendor-app.exe') -Force
Copy-Item -LiteralPath $vendor -Destination (Join-Path $tpPkg 'payload\vendor-app.exe') -Force
$tpSources = @([pscustomobject]@{ path = 'payload/vendor-app.exe'; source = 'vendor/payload/vendor-app.exe';
                                  sha256 = (Get-Hash (Join-Path $tpsrc 'payload\vendor-app.exe')); third_party = $true })
$line = Test-PackageIdentity -PackageRoot $tpPkg -Sources $tpSources -SignerThumbprints $pinned -Python $python -Root $base
Assert ($line -match '1 third-party') "a third-party row keeps its vendor signer: $line"

# 6. The same vendor file exchanged for one our publisher signed (a swap between files) is refused.
Copy-Item -LiteralPath $vendor -Destination (Join-Path $tpPkg 'payload\vendor-app.exe') -Force
Invoke-RunSign (Join-Path $tpPkg 'payload\vendor-app.exe') $publisher
$out = Invoke-Gate -Src $tpSources -Pkg $tpPkg
Assert (($out -match 'byte-equal to the registered' -or $out -match 'keeps its vendor signature') -and $out -match 'THREW') 'a swapped third-party file is refused'

# 7. The right image with a re-cut signature blob (HashMismatch) is refused.
$recutSrc = Join-Path $base 'recut'
New-Item -ItemType Directory -Force (Join-Path $recutSrc 'payload') | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'payload\app.exe') -Destination (Join-Path $recutSrc 'payload\app.exe') -Force
Copy-Item -LiteralPath (Join-Path $build 'payload\data.bin') -Destination (Join-Path $recutSrc 'payload\data.bin') -Force
Copy-Item -LiteralPath (Join-Path $build 'payload\app.exe') -Destination (Join-Path $recutSrc 'payload\app.exe') -Force
Invoke-RunSign (Join-Path $recutSrc 'payload\app.exe') $publisher
$bytes = [IO.File]::ReadAllBytes((Join-Path $recutSrc 'payload\app.exe'))
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$opt = $pe + 24
$magic = [BitConverter]::ToUInt16($bytes, $opt)
$dd = $opt + $(if ($magic -eq 0x20b) { 112 } else { 96 })
$secOff = [BitConverter]::ToInt32($bytes, $dd + 4 * 8)
$secSize = [BitConverter]::ToInt32($bytes, $dd + 4 * 8 + 4)
$bytes[$secOff + [int]($secSize / 2)] = [byte](($bytes[$secOff + [int]($secSize / 2)] + 1) % 256)
[IO.File]::WriteAllBytes((Join-Path $recutSrc 'payload\app.exe'), $bytes)
$sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $recutSrc 'payload\app.exe')
# On a host that does not trust the run's certificate the status reads UnknownError whether the blob is right
# or cut, so the gate's HashMismatch branch shows only with a trusted chain. The observable control is that the
# re-cut file is refused at all; the reason is printed.
"note the re-cut blob reads $($sig.Status) on this host"
$recutSources = @([pscustomobject]@{ path = 'payload/app.exe'; source = 'recut/payload/app.exe'; sha256 = (Get-Hash (Join-Path $recutSrc 'payload\app.exe')) })
$out = Invoke-Gate -Src $recutSources -Pkg $recutSrc
"the re-cut refusal: $(($out -split [Environment]::NewLine | Where-Object { $_ -match 'REFUSE' }) -join ' | ')"
Assert ($out -match 'THREW') 'a re-cut signature blob is refused'

# 8. A PE of the package that no row names (with -DriverMembers empty) is refused.
Copy-Item -LiteralPath $stub -Destination (Join-Path $pkg 'payload\extra.exe') -Force
$out = Invoke-Gate -Members @([pscustomobject]@{ source = 'payload/data.bin'; path = 'payload/data.bin' })
Assert ($out -match 'no source row or driver package member' -and $out -match 'THREW') 'a stray PE is refused (L2 sweep)'
Remove-Item -LiteralPath (Join-Path $pkg 'payload\extra.exe') -Force
"PASS package-identity gate: $passed checks"
