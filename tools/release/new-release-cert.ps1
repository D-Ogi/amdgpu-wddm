# Creates the release test-signing certificate for tester packages, once, outside the repository.
# The private key stays in <BC250_ROOT>\secrets\release and never enters a package or a commit: build-release.ps1
# signs with the PFX there and copies only the public .cer into the package. The release certificate is separate from
# the lab certificate (CN=BC-250 lab test), so a tester PC never trusts the lab's signing key and vice versa.
# Needs openssl.exe on PATH (Git for Windows or MSYS2 ship one). Refuses to overwrite an existing certificate.
param([string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
      [int]$Days = 1095)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $Root 'secrets\release'
$pfx = Join-Path $dir 'amdgpu-wddm-release.pfx'
$cer = Join-Path $dir 'amdgpu-wddm-release.cer'
$pass = Join-Path $dir 'amdgpu-wddm-release.pass'
if (Test-Path -LiteralPath $pfx) { throw "$pfx exists: a release certificate is already there, nothing changed" }
$openssl = (Get-Command openssl -ErrorAction Stop).Source
[void][IO.Directory]::CreateDirectory($dir)
$tmp = Join-Path $dir ('tmp-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($tmp)
try {
    # 32 random bytes as hex: the PFX password, read by build-release.ps1 from the file and never printed.
    $bytes = New-Object byte[] 32
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    [IO.File]::WriteAllText($pass, (($bytes | ForEach-Object { $_.ToString('x2') }) -join ''))
    $cnf = Join-Path $tmp 'cert.cnf'
    [IO.File]::WriteAllText($cnf, @"
[req]
distinguished_name = dn
prompt = no
x509_extensions = ext
[dn]
CN = amdgpu-wddm tester release (test signing)
O = amdgpu-wddm project
[ext]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
"@)
    $key = Join-Path $tmp 'key.pem'
    $crt = Join-Path $tmp 'cert.pem'
    & $openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days $Days -config $cnf -keyout $key -out $crt 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "openssl req failed ($LASTEXITCODE)" }
    # SHA1-3DES PBE: the PKCS#12 encryption every signtool and Windows build imports.
    & $openssl pkcs12 -export -inkey $key -in $crt -out $pfx -passout "file:$pass" -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "openssl pkcs12 failed ($LASTEXITCODE)" }
    & $openssl x509 -in $crt -outform DER -out $cer 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "openssl x509 failed ($LASTEXITCODE)" }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
$c = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
"release certificate created: subject '$($c.Subject)', thumbprint $($c.Thumbprint), valid to $($c.NotAfter.ToString('yyyy-MM-dd'))"
"public part: $cer (the private key and its password stay in $dir)"
