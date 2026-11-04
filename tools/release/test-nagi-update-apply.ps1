# End-to-end host test of setup --apply (installer\apply.ps1, docs/design/self-update.md) on a scratch install root:
# signed update manifests of this run, apps packs with a small stand-in control application, and a stand-in release
# package. apply.ps1 runs in Windows PowerShell 5.1 as setup.exe starts it, with -NoAdminCheck (no registry, no
# protected folder) and the run's signer as the pinned list. The cases follow one install through its updates:
#   an apps update (the classic folder becomes the junction), the same update again, a downgrade, an apps pack for a
#   driver ABI that is not installed, a manifest of a signer that is not pinned, an apps pack with one changed byte,
#   new apps that do not start (the switch goes back), a release package whose manifest is not the signed one, and a
#   driver update (the transaction and the handoff to the package's setup window).
#
#   pwsh -File tools\release\test-nagi-update-apply.ps1 -WorkRoot <folder>
#
# Exit 0 = every check passed.
param([Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'run this test under pwsh 7 (it signs with SignedCms of .NET and starts Windows PowerShell 5.1 for apply.ps1)' }
$apply = Join-Path $PSScriptRoot 'installer\apply.ps1'
$script:pass = 0; $script:fail = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { $script:pass++; Write-Host "PASS $What" } else { $script:fail++; Write-Host "FAIL $What" }
}
$WorkRoot = (New-Item -ItemType Directory -Force $WorkRoot).FullName
$run = Join-Path $WorkRoot ([Guid]::NewGuid().ToString('N').Substring(0, 8))
[void][IO.Directory]::CreateDirectory($run)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function Sha([byte[]]$Bytes) { return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)) }
function New-RunCert([string]$Name) {
    $rsa = [Security.Cryptography.RSA]::Create(2048)
    $req = [Security.Cryptography.X509Certificates.CertificateRequest]::new("CN=$Name", $rsa,
        [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    return $req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddYears(1))
}
$publisher = New-RunCert 'Nagi apply test publisher'
$stranger = New-RunCert 'Nagi apply test stranger'
function Protect-Bytes([byte[]]$Data, $Cert) {
    $cms = [Security.Cryptography.Pkcs.SignedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($Data), $true)
    $signer = [Security.Cryptography.Pkcs.CmsSigner]::new($Cert)
    $signer.DigestAlgorithm = [Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.2.1')
    $cms.ComputeSignature($signer)
    return $cms.Encode()
}

# The stand-in control application: --version prints a line and exits with the code it was built with.
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
function New-App([string]$Name, [int]$Code) {
    $src = Join-Path $run "$Name.cs"
    [IO.File]::WriteAllText($src, "class P { static int Main(string[] a) { if (a.Length == 1 && a[0] == `"--version`") { System.Console.WriteLine(`"$Name`"); return $Code; } return 0; } }")
    $exe = Join-Path $run "$Name.exe"
    $out = & $csc /nologo /target:exe "/out:$exe" $src 2>&1
    if ($LASTEXITCODE) { throw "csc: $out" }
    return [IO.File]::ReadAllBytes($exe)
}
$goodApp = New-App 'nagi-test-app-good' 0
$badApp = New-App 'nagi-test-app-bad' 3

$abi = '0x000700D8'; $cat = 'c' * 64
$exeName = 'amdgpu_wddm_control.exe'
function New-Zip([string]$Path, [hashtable]$Entries) {
    $z = [IO.Compression.ZipFile]::Open($Path, 'Create')
    try { foreach ($k in ($Entries.Keys | Sort-Object)) { $e = $z.CreateEntry($k); $s = $e.Open(); $b = [byte[]]$Entries[$k]; $s.Write($b, 0, $b.Length); $s.Dispose() } }
    finally { $z.Dispose() }
}
# One update folder as the control application fills it: the manifest, its signature and the named assets.
function New-Update {
    param([string]$Name, [string]$Release, [string]$DriverCid, [string]$AppsCid, [byte[]]$App = $goodApp,
        [string[]]$AppsAbis = @($abi), [string]$MinInstalled = '0.7.216.109-tester.37', $Signer = $publisher,
        [switch]$Driver, [switch]$TamperPack, [switch]$WrongPackageManifest)
    $dir = Join-Path $run $Name; [void][IO.Directory]::CreateDirectory($dir)
    $files = @{ $exeName = $App; 'readme.txt' = [Text.Encoding]::UTF8.GetBytes("apps $Release") }
    $packName = "nagi-apps-$Release.zip"
    New-Zip (Join-Path $dir $packName) $files
    $packBytes = [IO.File]::ReadAllBytes((Join-Path $dir $packName))
    $packHash = Sha $packBytes
    if ($TamperPack) {
        # One changed byte of the first stored entry's data: the size stays, the SHA-256 does not.
        $packBytes[40] = [byte]($packBytes[40] -bxor 0xFF); [IO.File]::WriteAllBytes((Join-Path $dir $packName), $packBytes)
    }
    $driverRow = [ordered]@{ name = 'driver'; kind = 'driver'; version = $Release; asset = "amdgpu-wddm-tester-$Release.zip"; sha256 = ('0' * 64); size = 1
        restart = $true; content_id = $DriverCid
        driver = [ordered]@{ kmd_version = '0.7.216.1'; kmd_abi = $abi; kmd_sha256 = ('9' * 64); state_schema = 2; catalog_id = $cat; package_manifest_sha256 = $null } }
    if ($Driver) {
        $pkgManifest = [Text.Encoding]::UTF8.GetBytes("{ `"release`": `"$Release`" }")
        $top = "amdgpu-wddm-tester-$Release"
        $zip = Join-Path $dir $driverRow.asset
        New-Zip $zip @{ "$top/setup.exe" = $goodApp; "$top/files/manifest.json" = $pkgManifest }
        $zb = [IO.File]::ReadAllBytes($zip)
        $driverRow.sha256 = Sha $zb; $driverRow.size = $zb.Length
        $driverRow.driver.package_manifest_sha256 = $(if ($WrongPackageManifest) { 'E' * 64 } else { Sha $pkgManifest })
    }
    $m = [ordered]@{ schema = 'nagi.update/1'; product = 'Nagi'; release = $Release; flavour = 'release'; built_utc = '2026-11-04T00:00:00Z'
        min_installed = $MinInstalled
        components = @($driverRow,
            [ordered]@{ name = 'apps'; kind = 'apps'; version = $Release; asset = $packName; sha256 = $packHash; size = $packBytes.Length; restart = $false
                content_id = $AppsCid
                apps = [ordered]@{ kmd_abis = $AppsAbis; state_schemas = @(2)
                    files = @($files.Keys | Sort-Object | ForEach-Object { [ordered]@{ path = $_; sha256 = (Sha $files[$_]); size = $files[$_].Length } }) } }) }
    $mb = [Text.UTF8Encoding]::new($false).GetBytes(($m | ConvertTo-Json -Depth 8))
    [IO.File]::WriteAllBytes((Join-Path $dir 'nagi-update.json'), $mb)
    [IO.File]::WriteAllBytes((Join-Path $dir 'nagi-update.json.p7s'), (Protect-Bytes $mb $Signer))
    return $dir
}

# The install root of a full tester.37 install: manifest.json with its update identities and the classic control folder.
$root = Join-Path $run 'install'; $updateRoot = Join-Path $run 'update'
$control = Join-Path $root 'control'; [void][IO.Directory]::CreateDirectory($control)
[IO.File]::WriteAllBytes((Join-Path $control $exeName), $goodApp)
[IO.File]::WriteAllText((Join-Path $control 'readme.txt'), 'apps of the full install')
$D1 = '1' * 64; $D2 = '2' * 64; $A1 = 'A' * 64; $A2 = 'B' * 64; $A3 = 'C' * 64
$installManifest = [ordered]@{ release = '0.7.216.109-tester.37'; kmd_abi = $abi; control_app_exe = $exeName
    components = @([ordered]@{ package_path = 'payload/kmd/bc250kmd.sys'; sha256 = ('8' * 64) })
    update_components = [ordered]@{ driver = [ordered]@{ content_id = $D1; files = 90; state_schema = 2; catalog_id = $cat }; apps = [ordered]@{ content_id = $A1; files = 6 } } }
[IO.File]::WriteAllText((Join-Path $root 'manifest.json'), ($installManifest | ConvertTo-Json -Depth 6))

$oldPath = $env:PSModulePath
$env:PSModulePath = "$env:ProgramFiles\WindowsPowerShell\Modules;$env:WINDIR\system32\WindowsPowerShell\v1.0\Modules"
function Invoke-Apply([string]$Dir) {
    $result = Join-Path $run ("result-" + [Guid]::NewGuid().ToString('N').Substring(0, 6) + '.json')
    $out = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $apply -UpdateDir $Dir -ResultFile $result `
        -InstallRoot $root -UpdateRoot $updateRoot -NoAdminCheck -Pinned $publisher.Thumbprint 2>&1
    $code = $LASTEXITCODE
    $r = $null
    if (Test-Path -LiteralPath $result) { $r = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json }
    if (-not $r) { Write-Host ($out -join "`n") }
    return [pscustomobject]@{ code = $code; r = $r }
}
function Get-Link { $i = Get-Item -LiteralPath $control -Force; if ($i.LinkType -eq 'Junction') { return Split-Path -Leaf ([string]$i.Target) } return '(classic)' }
function Show($x) { return "exit $($x.code), $($x.r.outcome) $($x.r.reason) $($x.r.message); $($x.r.detail)" }

try {
    $t38 = '0.7.216.110-tester.38'; $t39 = '0.7.216.111-tester.39'
    $x = Invoke-Apply (New-Update 'u38' $t38 $D1 $A2)
    $rec = Get-Content -LiteralPath (Join-Path $updateRoot 'installed.json') -Raw | ConvertFrom-Json
    Check ($x.code -eq 0 -and $x.r.outcome -eq 'applied' -and $x.r.message -eq 'apply.done' -and (Get-Link) -eq $t38 -and
        (Test-Path -LiteralPath (Join-Path $root 'apps\0.7.216.109-tester.37-installed\readme.txt')) -and
        $rec.release -eq $t38 -and $rec.apps.folder -eq $t38 -and $rec.apps.content_id -eq $A2 -and -not (Test-Path -LiteralPath (Join-Path $updateRoot 'transaction.json'))) `
        "apps update: the classic folder becomes apps\0.7.216.109-tester.37-installed, control is the junction to apps\$t38, no transaction ($(Show $x))"
    $x = Invoke-Apply (Join-Path $run 'u38')
    Check ($x.code -eq 0 -and $x.r.outcome -eq 'up-to-date' -and (Get-Link) -eq $t38) "the same update again: up to date ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u36' '0.7.216.108-tester.36' $D1 $A3 -MinInstalled '0.7.216.100-tester.20')
    Check ($x.code -eq 2 -and $x.r.reason -eq 'downgrade' -and $x.r.message -eq 'apply.refused-downgrade' -and (Get-Link) -eq $t38) "an older release is refused ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u39abi' $t39 $D1 $A3 -AppsAbis @('0x000700D9'))
    Check ($x.code -eq 2 -and $x.r.reason -eq 'incompatible' -and (Get-Link) -eq $t38) "apps for another driver ABI are refused ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u39sig' $t39 $D1 $A3 -Signer $stranger)
    Check ($x.code -eq 2 -and $x.r.reason -eq 'signature' -and $x.r.message -eq 'apply.refused-signature' -and (Get-Link) -eq $t38) "a signer that is not pinned is refused ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u39hash' $t39 $D1 $A3 -TamperPack)
    Check ($x.code -eq 2 -and $x.r.reason -eq 'hash' -and (Get-Link) -eq $t38 -and -not (Test-Path -LiteralPath (Join-Path $root "apps\$t39"))) "an apps pack with one changed byte is refused, nothing unpacked ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u39bad' $t39 $D1 $A3 -App $badApp)
    $rec = Get-Content -LiteralPath (Join-Path $updateRoot 'installed.json') -Raw | ConvertFrom-Json
    Check ($x.code -eq 3 -and $x.r.outcome -eq 'rolled-back' -and $x.r.message -eq 'apply.rolled-back' -and (Get-Link) -eq $t38 -and $rec.apps.folder -eq $t38) "new apps that do not start: the switch goes back to apps\$t38 ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u39pm' $t39 $D2 $A3 -Driver -WrongPackageManifest)
    Check ($x.code -eq 2 -and $x.r.reason -eq 'hash' -and -not (Test-Path -LiteralPath (Join-Path $updateRoot 'transaction.json'))) "a release package whose manifest is not the signed one is refused, no transaction ($(Show $x))"
    $x = Invoke-Apply (New-Update 'u39drv' $t39 $D2 $A3 -Driver)
    $txn = $null; $tp = Join-Path $updateRoot 'transaction.json'
    if (Test-Path -LiteralPath $tp) { $txn = Get-Content -LiteralPath $tp -Raw | ConvertFrom-Json }
    Check ($x.code -eq 0 -and $x.r.outcome -eq 'handoff' -and $x.r.restart -and $x.r.message -eq 'apply.driver-handoff' -and
        $x.r.handoff_setup -like "*\driver\$t39\amdgpu-wddm-tester-$t39\setup.exe" -and (Test-Path -LiteralPath $x.r.handoff_setup) -and
        $txn.state -eq 'driver-pending' -and $txn.from.release -eq '0.7.216.109-tester.37' -and $txn.from.kmd_sha256 -eq ('8' * 64) -and
        $txn.from.apps_folder -eq $t38 -and $txn.apps_rollback -eq $true -and $txn.to.release -eq $t39 -and $txn.to.catalog_id -eq $cat -and
        (Get-Link) -eq $t38) "a driver update: the transaction from the running driver and apps, the handoff to the package's setup ($(Show $x))"
} finally {
    $env:PSModulePath = $oldPath
    # The junctions first, as links: a recursive delete must never walk into their targets.
    foreach ($l in @(Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue | Where-Object { $_.LinkType -eq 'Junction' })) { [IO.Directory]::Delete($l.FullName) }
    Remove-Item -LiteralPath $run -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "nagi-update-apply: $script:pass passed, $script:fail failed"
if ($script:fail) { exit 1 }
exit 0
