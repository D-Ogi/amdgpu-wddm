# Builds the tester release package: one folder and its zip that a tester unpacks and runs (install.cmd).
#
#   pwsh -File tools\release\build-release.ps1 [-Root <BC250_ROOT>] [-Out <dir>] [-Version <v>]
#
# Inputs are the artifacts listed in release-sources.json (registered lab drivers, control application, tools,
# firmware), each refused unless its SHA256 matches. The KMD package is re-signed with the release test certificate
# (new-release-cert.ps1, private key in a private directory outside the repository, never copied): the .sys gets the release
# signature in place of the lab one (its code and its Authenticode hash do not change), Inf2Cat makes a new catalog,
# and the catalog is signed. The package holds the public .cer only.
# manifest.json lists every file of the package with its SHA256, and every installed component with its role,
# install location, file version and SHA256 (the control application reads it from the install root).
# Gates: every source hash, signer of .sys and .cat = the release certificate, no private-key material in the
# package, every installer script parses under Windows PowerShell 5.1. Nothing here opens a window: child processes
# run with CreateNoWindow and redirected output.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string]$Out,
    [string]$DriverVer = '0.7.197.100',          # the release's own 4th field: ranks above the lab's x.y.z.1, names the package
    [string]$Version = '0.7.197.100-tester.1',
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'headless.ps1')
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
    '  {0}  {1,-12} {2}' -f $h.Substring(0, 8), $f.component, $f.path
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
Copy-Item -LiteralPath (Join-Path $repo 'docs\testing\TESTERS.md') -Destination (Join-Path $pkg 'TESTERS.md')
foreach ($f in 'LICENSE.md', 'NOTICE') { Copy-Item -LiteralPath (Join-Path $repo $f) -Destination $pkg }
# The release's own third-party list (components of this package, exact commits) and the licence texts it names.
Copy-Item -LiteralPath (Join-Path $repo 'docs\testing\THIRD-PARTY.md') -Destination (Join-Path $pkg 'THIRD-PARTY.md')
$lic = Join-Path $pkg 'licenses'
[void][IO.Directory]::CreateDirectory($lic)
Copy-Item -Path (Join-Path $repo 'docs\testing\licenses\*') -Destination $lic
Copy-Item -LiteralPath (Join-Path $pkg 'payload\firmware\LICENSE.amdgpu') -Destination $lic
foreach ($m in [regex]::Matches((Get-Content -LiteralPath (Join-Path $pkg 'THIRD-PARTY.md') -Raw), '`([\w.-]+\.(?:txt|md)|LICENSE\.amdgpu)`')) {
    $n = $m.Groups[1].Value
    if ($n -notin 'LICENSE.md', 'NOTICE' -and -not (Test-Path -LiteralPath (Join-Path $lic $n))) { throw "THIRD-PARTY.md names licenses\$n, which is missing" }
}
# tester.1-6 shipped a cgit "Not found" page as LICENSE.amdgpu: no licence text may be a web page.
foreach ($f in Get-ChildItem -LiteralPath $lic -File) {
    if ((Get-Content -LiteralPath $f.FullName -TotalCount 3) -match '(?i)<!DOCTYPE|<html') { throw "licenses\$($f.Name) is a web page, not a licence text" }
}

Write-Host 'driver version'
# DriverVer = <date>,$DriverVer in the packaged INF, nothing else changes; the catalog below is made from this INF.
$infPath = Join-Path $pkg 'payload\kmd\bc250kmd.inf'
$infText = [IO.File]::ReadAllText($infPath)
$rx = [regex]'(?m)^(DriverVer\s*=\s*[\d/]+,)(\d+\.\d+\.\d+\.\d+)(\s*)$'
if ($rx.Matches($infText).Count -ne 1) { throw 'bc250kmd.inf: expected exactly one DriverVer line' }
$infText = $rx.Replace($infText, { param($m) $m.Groups[1].Value + $DriverVer + $m.Groups[3].Value })
[IO.File]::WriteAllText($infPath, $infText)
if ($infText -notmatch ('(?m)^DriverVer\s*=\s*[\d/]+,' + [regex]::Escape($DriverVer) + '\s*$')) { throw 'DriverVer rewrite failed' }
'  DriverVer {0}' -f $DriverVer

Write-Host 'sign'
$kmd = Join-Path $pkg 'payload\kmd'
$certDir = Join-Path $pkg 'payload\cert'
[void][IO.Directory]::CreateDirectory($certDir)
Copy-Item -LiteralPath $cer -Destination (Join-Path $certDir 'amdgpu-wddm-release.cer')
$release = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
$pass = (Get-Content -LiteralPath $passFile -Raw).Trim()
function Invoke-Sign([string]$Path) {
    $r = Invoke-Headless -File $signtool -Arguments @('sign', '/fd', 'SHA256', '/f', $pfx, '/p', $pass, $Path) -TimeoutSeconds 120
    if ($r.code -ne 0) { throw "signtool sign $Path failed: $($r.text -replace [regex]::Escape($pass), '<redacted>')" }
}
Invoke-Sign (Join-Path $kmd 'bc250kmd.sys')
$r = Invoke-Headless -File $inf2cat -Arguments @("/driver:$kmd", '/os:10_X64', '/uselocaltime') -TimeoutSeconds 300
if ($r.code -ne 0) { throw "Inf2Cat failed: $($r.text)" }
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
$r = Invoke-Headless -File $ps51 -Arguments @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'test-parse51.ps1'), '-Directory', $inst) -TimeoutSeconds 120
$r.text
if ($r.code -ne 0) { throw 'a script does not parse under Windows PowerShell 5.1' }

Write-Host 'manifest'
# Where each payload directory lands on the tester PC (install.ps1 does the copying).
function Get-InstallLocation([string]$PackagePath) {
    $leaf = Split-Path $PackagePath -Leaf
    switch -Regex ($PackagePath) {
        '^payload/kmd/' { return "DriverStore (bc250kmd.inf)\$leaf" }
        '^payload/system32/' { return "%SystemRoot%\System32\$leaf" }
        '^payload/firmware/' { return "C:\BC250\firmware\$leaf" }
        '^payload/cert/' { return 'LocalMachine Root and TrustedPublisher' }
        '^payload/(\w+)/' { return "<InstallDir>\$($Matches[1])\$leaf" }
    }
    return $null
}
$components = @()
foreach ($f in $sources.files + @([pscustomobject]@{ component = 'tool'; path = 'payload/tools/start-confirm.ps1' }, [pscustomobject]@{ component = 'tool'; path = 'payload/tools/start-confirm-core.ps1' }, [pscustomobject]@{ component = 'certificate'; path = 'payload/cert/amdgpu-wddm-release.cer' })) {
    $p = Join-Path $pkg ($f.path -replace '/', '\')
    $ver = (Get-Item -LiteralPath $p).VersionInfo.FileVersion
    $components += [ordered]@{ role = $f.component; package_path = $f.path; install_path = (Get-InstallLocation $f.path); version = $(if ($ver) { $ver.Trim() } else { $null }); sha256 = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
}
$files = @()
foreach ($f in Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($pkg.Length + 1) -replace '\\', '/'
    $files += [ordered]@{ path = $rel; sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash; size = $f.Length }
}
$manifest = [ordered]@{
    schema = 1
    name = $name
    release = $Version
    version = $Version
    kmd_version = $DriverVer
    kmd_build = $sources.kmd_version
    kmd_abi = $sources.kmd_abi             # BC250_KMD_VERSION, what bc250kmd_cli info reports (start-confirm, verify)
    built_utc = [DateTime]::UtcNow.ToString('o')
    release_certificate = $release.Thumbprint
    control_app_exe = $sources.control_app_exe
    sources = $sources.sources
    components = $components
    files = $files
}
[IO.File]::WriteAllText((Join-Path $pkg 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5))
Compress-Archive -Path $pkg -DestinationPath $zip
'{0} files, package {1}' -f $files.Count, $pkg
'zip {0} SHA256 {1}' -f $zip, (Get-FileHash -LiteralPath $zip).Hash
