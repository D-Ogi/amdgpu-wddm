# Host only: exercise real embedded signatures, image pins and a changed-code
# negative control. No trust-store mutation and no execution of the test image.
param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Image,
    [Parameter(Mandatory)][string]$Out,
    [ValidateSet('pinned-test','system')][string]$ExpectedRoot = 'pinned-test')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
. (Join-Path $PSScriptRoot 'driver-signature.ps1')
if (Test-Path -LiteralPath $Out) { throw 'use a fresh signing proof directory' }
[void][IO.Directory]::CreateDirectory($Out)
$images = Join-Path $Out 'images'
[void][IO.Directory]::CreateDirectory($images)
$copy = Join-Path $images 'probe.dll'
Copy-Item -LiteralPath $Image -Destination $copy
$python = (Get-Command python).Source
$identity = Join-Path $PSScriptRoot 'image_identity.py'
$pins = Join-Path $Out 'images.json'
function Check-Identity([string]$Operation) {
    $r = Invoke-Headless -File $python -Arguments @($identity,'--directory',$images,$Operation,$pins) -TimeoutSeconds 60
    if ($r.code -ne 0) { throw $r.text }
}
Check-Identity '--write'
$sign = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe'
$pfx = Join-Path $Root 'secrets\release\amdgpu-wddm-release.pfx'
$cer = Join-Path $Root 'secrets\release\amdgpu-wddm-release.cer'
$pass = (Get-Content -LiteralPath (Join-Path $Root 'secrets\release\amdgpu-wddm-release.pass') -Raw).Trim()
try {
    foreach ($round in 1,2) {
        $r = Invoke-Headless -File $sign -Arguments @('sign','/fd','SHA256','/f',$pfx,'/p',$pass,$copy) -TimeoutSeconds 60
        if ($r.code -ne 0) { throw ($r.text.Replace($pass,'<redacted>')) }
        Test-DriverFileSignature -Path $copy -SignTool $sign -ExpectedRoot $ExpectedRoot -TestCertificate $cer
        Check-Identity '--check'
        Write-Output "PASS image identity and trusted embedded signature after signing round $round"
    }
} finally { $pass = $null }
if ($ExpectedRoot -eq 'pinned-test') {
    # A valid signature under a different root must not pass the pinned policy.
    # This key exists in memory only; no certificate store is opened.
    $key = [Security.Cryptography.RSA]::Create(2048)
    try {
        $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
            'CN=Unrelated test root', $key, [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $other = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-1), [DateTimeOffset]::UtcNow.AddHours(1))
        try {
            $wrongRoot = Join-Path $Out 'unrelated-root.cer'
            [IO.File]::WriteAllBytes($wrongRoot, $other.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
            $refused = $false
            try { Test-DriverFileSignature -Path $copy -SignTool $sign -ExpectedRoot pinned-test -TestCertificate $wrongRoot } catch { $refused = $true }
            if (-not $refused) { throw 'signature accepted under an unrelated pinned root' }
            'PASS unrelated pinned root refused'
        } finally { $other.Dispose() }
    } finally { $key.Dispose() }
}
# Change an instruction byte in the first section, leaving PE headers parseable.
$bytes = [IO.File]::ReadAllBytes($copy)
$pe = [BitConverter]::ToInt32($bytes,0x3c)
$section = $pe + 24 + [BitConverter]::ToUInt16($bytes,$pe + 20)
$raw = [BitConverter]::ToUInt32($bytes,$section + 20)
if ($raw -lt $section + 40 -or $raw -ge $bytes.Length) { throw 'unexpected fixture section layout' }
$bytes[$raw] = $bytes[$raw] -bxor 1
[IO.File]::WriteAllBytes($copy,$bytes)
$r = Invoke-Headless -File $python -Arguments @($identity,'--directory',$images,'--check',$pins) -TimeoutSeconds 60
if ($r.code -eq 0) { throw 'modified code accepted by image identity check' }
$refused = $false
try { Test-DriverFileSignature -Path $copy -SignTool $sign -ExpectedRoot $ExpectedRoot -TestCertificate $cer } catch { $refused = $true }
if (-not $refused) { throw 'modified code accepted by signature verification' }
'PASS changed code refused by both image pins and signature verification'
