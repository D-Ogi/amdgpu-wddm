# Builds the tester release package: one folder and its zip that a tester unpacks and runs (install.cmd).
#
#   pwsh -File tools\release\build-release.ps1 [-Root <BC250_ROOT>] [-Out <dir>] [-ControlApp <dir> -ControlAppExe <name>]
#
# Inputs are the registered lab artifacts listed in release-sources.json, each refused unless its SHA256 matches.
# The KMD package is re-signed with the release test certificate (new-release-cert.ps1, private key in
# <BC250_ROOT>\secrets\release, never copied): the .sys gets the release signature in place of the lab one (its code
# and its Authenticode hash do not change), Inf2Cat makes a new catalog, and the catalog is signed. The package
# holds the public .cer only. manifest.json lists every file of the package with its SHA256.
# Gates: every source hash, signer of .sys and .cat = the release certificate, no private-key material in the
# package, every installer script parses under Windows PowerShell 5.1.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string]$Out,
    [string]$Version = '0.7.197.1-tester.0',
    [string]$ControlApp,
    [string]$ControlAppExe,
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = Join-Path $Root 'scratch\release\out' }
$repo = Split-Path (Split-Path $PSScriptRoot)
$name = "amdgpu-wddm-tester-$Version"
$pkg = Join-Path $Out $name
$zip = "$pkg.zip"
$sources = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-sources.json') -Raw | ConvertFrom-Json
$secrets = Join-Path $Root 'secrets\release'
$pfx = Join-Path $secrets 'amdgpu-wddm-release.pfx'
$cer = Join-Path $secrets 'amdgpu-wddm-release.cer'
$passFile = Join-Path $secrets 'amdgpu-wddm-release.pass'
foreach ($f in $pfx, $cer, $passFile) { if (-not (Test-Path -LiteralPath $f)) { throw "missing ${f}: run tools\release\new-release-cert.ps1 once" } }
$wdk = Join-Path $Root 'toolchain\nuget\microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
$inf2cat = Join-Path $wdk "bin\$KitVersion\x86\Inf2Cat.exe"
foreach ($t in $signtool, $inf2cat) { if (-not (Test-Path -LiteralPath $t)) { throw "missing tool $t" } }
if ($ControlApp -and -not $ControlAppExe) { throw '-ControlApp needs -ControlAppExe (the entry exe name)' }

if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
[void][IO.Directory]::CreateDirectory($pkg)

Write-Host "sources ($(@($sources.files).Count))"
foreach ($f in $sources.files) {
    $src = Join-Path $Root ($f.source -replace '/', '\')
    $h = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
    if ($h -ne $f.sha256) { throw "source hash mismatch: $($f.source) is $h, release-sources.json says $($f.sha256)" }
    $dst = Join-Path $pkg ($f.path -replace '/', '\')
    [void][IO.Directory]::CreateDirectory((Split-Path $dst))
    Copy-Item -LiteralPath $src -Destination $dst
    '  {0}  {1}' -f $h.Substring(0, 8), $f.path
}

Write-Host 'installer and documents'
$inst = Join-Path $pkg 'installer'
[void][IO.Directory]::CreateDirectory($inst)
foreach ($f in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'installer') -File) {
    if ($f.Extension -eq '.cmd') { Copy-Item -LiteralPath $f.FullName -Destination $pkg } else { Copy-Item -LiteralPath $f.FullName -Destination $inst }
}
# The installed copy of the start-confirm task runs from <install root>\tools next to bc250kmd_cli.exe.
foreach ($f in 'start-confirm.ps1', 'start-confirm-core.ps1') { Copy-Item -LiteralPath (Join-Path $inst $f) -Destination (Join-Path $pkg "payload\tools\$f") }
Copy-Item -LiteralPath (Join-Path $repo 'docs\testing\INSTALL.md') -Destination (Join-Path $pkg 'INSTALL.md')
foreach ($f in 'LICENSE.md', 'NOTICE', 'THIRD-PARTY.md') { Copy-Item -LiteralPath (Join-Path $repo $f) -Destination $pkg }
if ($ControlApp) {
    $dst = Join-Path $pkg 'payload\control'
    [void][IO.Directory]::CreateDirectory($dst)
    Copy-Item -Path (Join-Path $ControlApp '*') -Destination $dst -Recurse
    if (-not (Test-Path -LiteralPath (Join-Path $dst $ControlAppExe))) { throw "control app: ${ControlAppExe} not found in $ControlApp" }
}

Write-Host 'sign'
$kmd = Join-Path $pkg 'payload\kmd'
$certDir = Join-Path $pkg 'payload\cert'
[void][IO.Directory]::CreateDirectory($certDir)
Copy-Item -LiteralPath $cer -Destination (Join-Path $certDir 'amdgpu-wddm-release.cer')
$release = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
$pass = (Get-Content -LiteralPath $passFile -Raw).Trim()
function Invoke-Sign([string]$Path) {
    $o = & $signtool sign /fd SHA256 /f $pfx /p $pass $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "signtool sign $Path failed: $($o -replace [regex]::Escape($pass), '<redacted>')" }
}
Invoke-Sign (Join-Path $kmd 'bc250kmd.sys')
$o = & $inf2cat "/driver:$kmd" '/os:10_X64' '/uselocaltime' 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed: $o" }
Invoke-Sign (Join-Path $kmd 'bc250kmd.cat')
$pass = $null
foreach ($f in 'bc250kmd.sys', 'bc250kmd.cat') {
    $s = Get-AuthenticodeSignature -LiteralPath (Join-Path $kmd $f)
    if (-not $s.SignerCertificate -or $s.SignerCertificate.Thumbprint -ne $release.Thumbprint) { throw "$f is not signed by the release certificate $($release.Thumbprint)" }
    '  {0} signed by {1}' -f $f, $release.Thumbprint
}

Write-Host 'gates'
$forbidden = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.Extension -in '.pfx', '.pass', '.pem', '.key', '.p12', '.pvk' })
if ($forbidden.Count) { throw "private-key material in the package: $($forbidden.FullName -join ', ')" }
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$parse = Join-Path $PSScriptRoot 'test-parse51.ps1'
& $ps51 -NoProfile -ExecutionPolicy Bypass -File $parse -Directory $inst
if ($LASTEXITCODE -ne 0) { throw 'a script does not parse under Windows PowerShell 5.1' }

Write-Host 'manifest'
$files = @()
foreach ($f in Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($pkg.Length + 1) -replace '\\', '/'
    $files += [ordered]@{ path = $rel; sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash; size = $f.Length }
}
$manifest = [ordered]@{
    schema = 1
    name = $name
    version = $Version
    kmd_version = $sources.kmd_version
    built_utc = [DateTime]::UtcNow.ToString('o')
    release_certificate = $release.Thumbprint
    control_app_exe = $(if ($ControlApp) { $ControlAppExe } else { $null })
    sources = $sources.sources
    files = $files
}
[IO.File]::WriteAllText((Join-Path $pkg 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5))
Compress-Archive -Path $pkg -DestinationPath $zip
'{0} files, package {1}' -f $files.Count, $pkg
'zip {0} SHA256 {1}' -f $zip, (Get-FileHash -LiteralPath $zip).Hash
