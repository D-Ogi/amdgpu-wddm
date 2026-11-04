# Host test of the signature of the update manifest (docs/design/self-update.md): the signtool arguments that
# build-release.ps1 signs it with, and the check that the installed release runs on it (installer\nagi-update.ps1,
# Test-NagiManifestSignature, in Windows PowerShell 5.1 as the installer runs it). A code-signing certificate of this
# run only signs a manifest two ways: SignedCms, and signtool /p7 as the release runs it when signtool is there. Each
# signature must pass. Each negative control must fail: one changed byte of the manifest, one changed byte of the
# signature, a signer that is not pinned, a pinned list without the signer, and a signature over another file.
# No certificate store is opened. The PFX lives under -WorkRoot for the run and is deleted at the end.
#
#   pwsh -File tools\release\test-nagi-update-release.ps1 -WorkRoot <folder> [-SignTool <signtool.exe>]
#   pwsh -File tools\release\test-nagi-update-release.ps1 -WorkRoot <folder> -Regenerate
#       -Regenerate signs tools\release\test-data\nagi-update\nagi-update.json again with a new certificate of this run
#       and writes nagi-update.json.p7s and signer.txt beside it: the fixture of the control application's tests and
#       of test-nagi-update.ps1. The private key is deleted with the run.
#
# Exit 0 = every check passed.
param([Parameter(Mandatory)][string]$WorkRoot, [string]$SignTool, [switch]$Regenerate)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'run this test under pwsh 7 (it signs with SignedCms of .NET and starts Windows PowerShell 5.1 for the check)' }
. (Join-Path $PSScriptRoot 'user-mode-signing.ps1')
$installer = Join-Path $PSScriptRoot 'installer'
$fixtureDir = Join-Path $PSScriptRoot 'test-data\nagi-update'
$script:pass = 0; $script:fail = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { $script:pass++; Write-Host "PASS $What" } else { $script:fail++; Write-Host "FAIL $What" }
}
if (-not $SignTool -and $env:BC250_ROOT) { $SignTool = Join-Path $env:BC250_ROOT 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe' }
if (-not $SignTool) { $SignTool = Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot)))) 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe' }

$WorkRoot = (New-Item -ItemType Directory -Force $WorkRoot).FullName
$run = Join-Path $WorkRoot ([Guid]::NewGuid().ToString('N').Substring(0, 8))
[void][IO.Directory]::CreateDirectory($run)

# 1. The signtool arguments: the user-mode signer, /p7 into the folder, detached, content type data, no time stamp.
$testSigner = [pscustomobject]@{ mode = 'test'; thumbprint = 'A' * 40; timestamp_url = $null; store = $null; subject = $null }
$a = Get-ManifestSignToolArguments -Signer $testSigner -Path 'X:\o\nagi-update.json' -OutDir 'X:\o\p7' -Pfx 'X:\k.pfx' -Password 'pw'
Check (($a -join ' ') -eq 'sign /fd SHA256 /f X:\k.pfx /p pw /p7 X:\o\p7 /p7co 1.2.840.113549.1.7.1 /p7ce DetachedSignedData X:\o\nagi-update.json') "test route: $($a -join ' ')"
$storeSigner = [pscustomobject]@{ mode = 'store'; thumbprint = 'B' * 40; timestamp_url = 'http://time.example.test/'; store = 'LocalMachine'; subject = 'CN=x' }
$a = Get-ManifestSignToolArguments -Signer $storeSigner -Path 'X:\o\nagi-update.json' -OutDir 'X:\o\p7'
Check (($a -join ' ') -eq "sign /fd SHA256 /sha1 $('B' * 40) /s My /sm /p7 X:\o\p7 /p7co 1.2.840.113549.1.7.1 /p7ce DetachedSignedData X:\o\nagi-update.json") "store route without the time stamp: $($a -join ' ')"

# 2. Two certificates of this run: the publisher and a stranger.
function New-RunCert([string]$Name) {
    $rsa = [Security.Cryptography.RSA]::Create(2048)
    $req = [Security.Cryptography.X509Certificates.CertificateRequest]::new("CN=$Name", $rsa,
        [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $eku = [Security.Cryptography.OidCollection]::new(); [void]$eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
    $req.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
    return $req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddYears(5))
}
function Protect-Bytes([byte[]]$Data, $Cert) {
    $cms = [Security.Cryptography.Pkcs.SignedCms]::new([Security.Cryptography.Pkcs.ContentInfo]::new($Data), $true)
    $signer = [Security.Cryptography.Pkcs.CmsSigner]::new($Cert)
    $signer.DigestAlgorithm = [Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.2.1')
    $signer.IncludeOption = [Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $cms.ComputeSignature($signer)
    return $cms.Encode()
}
$publisher = New-RunCert 'Nagi update manifest test publisher'
$stranger = New-RunCert 'Nagi update manifest test stranger'

if ($Regenerate) {
    $m = Join-Path $fixtureDir 'nagi-update.json'
    [IO.File]::WriteAllBytes((Join-Path $fixtureDir 'nagi-update.json.p7s'), (Protect-Bytes ([IO.File]::ReadAllBytes($m)) $publisher))
    [IO.File]::WriteAllText((Join-Path $fixtureDir 'signer.txt'), $publisher.Thumbprint + "`n", [Text.UTF8Encoding]::new($false))
    Write-Host "regenerated: $fixtureDir signer $($publisher.Thumbprint)"
}

# 3. The check in Windows PowerShell 5.1: one child run reads a list of cases and answers each.
$manifest = Join-Path $run 'nagi-update.json'
Copy-Item -LiteralPath (Join-Path $fixtureDir 'nagi-update.json') -Destination $manifest
$bytes = [IO.File]::ReadAllBytes($manifest)
$cases = New-Object Collections.Generic.List[object]
function Add-Case([string]$Name, [string]$ManifestPath, [string]$SignaturePath, [string[]]$Pinned, [bool]$Expect) {
    $cases.Add([ordered]@{ name = $Name; manifest = $ManifestPath; signature = $SignaturePath; pinned = $Pinned; expect = $Expect })
}
function Add-Variants([string]$Route, [string]$Sig) {
    Add-Case "$Route signature, publisher pinned" $manifest $Sig @($publisher.Thumbprint) $true
    Add-Case "$Route signature, the pinned list of the release and the publisher" $manifest $Sig @('71B3BFDDB5EFC22F0C133AD55D865D48AD352981', $publisher.Thumbprint.ToLowerInvariant()) $true
    $t = [byte[]]$bytes.Clone(); $t[40] = [byte]($t[40] -bxor 0x01)
    $tm = Join-Path $run "$Route-tampered.json"; [IO.File]::WriteAllBytes($tm, $t)
    Add-Case "$Route negative control: one changed byte of the manifest" $tm $Sig @($publisher.Thumbprint) $false
    $s = [IO.File]::ReadAllBytes($Sig); $s[$s.Length - 20] = [byte]($s[$s.Length - 20] -bxor 0x01)
    $ts = Join-Path $run "$Route-tampered.p7s"; [IO.File]::WriteAllBytes($ts, $s)
    Add-Case "$Route negative control: one changed byte of the signature" $manifest $ts @($publisher.Thumbprint) $false
    Add-Case "$Route negative control: the publisher is not pinned" $manifest $Sig @($stranger.Thumbprint) $false
    Add-Case "$Route negative control: no pinned signer" $manifest $Sig @() $false
}
$cmsSig = Join-Path $run 'cms.p7s'
[IO.File]::WriteAllBytes($cmsSig, (Protect-Bytes $bytes $publisher))
Add-Variants 'SignedCms' $cmsSig
$strangerSig = Join-Path $run 'stranger.p7s'
[IO.File]::WriteAllBytes($strangerSig, (Protect-Bytes $bytes $stranger))
Add-Case 'negative control: a stranger signed it, the publisher is pinned' $manifest $strangerSig @($publisher.Thumbprint) $false
$otherSig = Join-Path $run 'other.p7s'
[IO.File]::WriteAllBytes($otherSig, (Protect-Bytes ([Text.Encoding]::UTF8.GetBytes('another file')) $publisher))
Add-Case 'negative control: the signature of another file' $manifest $otherSig @($publisher.Thumbprint) $false
Add-Case 'negative control: no signature file' $manifest (Join-Path $run 'absent.p7s') @($publisher.Thumbprint) $false
$fixtureSigner = (Get-Content -LiteralPath (Join-Path $fixtureDir 'signer.txt') -Raw).Trim()
$fixtureManifest = Join-Path $fixtureDir 'nagi-update.json'
Add-Case 'the committed fixture verifies with its signer' $fixtureManifest (Join-Path $fixtureDir 'nagi-update.json.p7s') @($fixtureSigner) $true

if (Test-Path -LiteralPath $SignTool -PathType Leaf) {
    $pfx = Join-Path $run 'publisher.pfx'
    $pw = [Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllBytes($pfx, $publisher.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $pw))
    try {
        $p7dir = Join-Path $run 'p7'
        [void][IO.Directory]::CreateDirectory($p7dir)
        $args7 = Get-ManifestSignToolArguments -Signer $testSigner -Path $manifest -OutDir $p7dir -Pfx $pfx -Password $pw
        $text = (& $SignTool @args7 2>&1 | ForEach-Object { [string]$_ }) -join ' / '
        Check ($LASTEXITCODE -eq 0) "signtool /p7 signs the manifest ($($text -replace [regex]::Escape($pw), '<redacted>'))"
        $p7 = Join-Path $p7dir 'nagi-update.json.p7'
        Check ((Test-Path -LiteralPath $p7) -and ([IO.File]::ReadAllBytes($manifest).Length -eq $bytes.Length)) 'signtool wrote nagi-update.json.p7 and left the manifest as it was'
        if (Test-Path -LiteralPath $p7) {
            # signtool writes the bare SignedData; the release wraps it into a ContentInfo (ConvertTo-CmsContentInfo).
            Add-Case 'negative control: the bare signtool output, not wrapped' $manifest $p7 @($publisher.Thumbprint) $false
            $p7s = Join-Path $run 'signtool.p7s'
            [IO.File]::WriteAllBytes($p7s, (ConvertTo-CmsContentInfo ([IO.File]::ReadAllBytes($p7))))
            Add-Variants 'signtool' $p7s
        }
    } finally { Remove-Item -LiteralPath $pfx -Force -ErrorAction SilentlyContinue; $pw = $null }
} else { Write-Host "NOTE no signtool at ${SignTool}: the signtool route is not run (pass -SignTool or set BC250_ROOT)" }

$caseFile = Join-Path $run 'cases.json'
[IO.File]::WriteAllText($caseFile, ($cases | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
$child = Join-Path $run 'check.ps1'
[IO.File]::WriteAllText($child, @'
param([string]$Installer, [string]$Cases, [string]$Answers)
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'nagi-update.ps1')
$out = New-Object Collections.ArrayList
$list = Get-Content -LiteralPath $Cases -Raw | ConvertFrom-Json   # 5.1 passes a JSON array down the pipeline as one object
foreach ($c in $list) {
    $r = Test-NagiManifestSignature -ManifestPath $c.manifest -SignaturePath $c.signature -Pinned @($c.pinned)
    [void]$out.Add([pscustomobject]@{ name = $c.name; ok = [bool]$r.ok; reason = $r.reason; version = $PSVersionTable.PSVersion.Major })
}
[IO.File]::WriteAllText($Answers, (ConvertTo-Json -InputObject $out.ToArray() -Depth 3), (New-Object Text.UTF8Encoding $false))
'@, [Text.UTF8Encoding]::new($false))
$ps51 = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
$answerFile = Join-Path $run 'answers.json'
& $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $child -Installer $installer -Cases $caseFile -Answers $answerFile | ForEach-Object { Write-Host "  5.1: $_" }
$answers = @()
if (Test-Path -LiteralPath $answerFile) { $answers = @(Get-Content -LiteralPath $answerFile -Raw | ConvertFrom-Json) }
Check ($answers.Count -eq $cases.Count -and @($answers | Where-Object { $_.version -ne 5 }).Count -eq 0) "Windows PowerShell 5.1 answered all $($cases.Count) cases ($($answers.Count))"
foreach ($c in $cases) {
    $ans = @($answers | Where-Object { $_.name -eq $c.name }) | Select-Object -First 1
    Check ($ans -and [bool]$ans.ok -eq $c.expect) "$($c.name): $(if ($ans) { if ($ans.ok) { 'accepted' } else { "refused ($($ans.reason))" } } else { 'no answer' })"
}

Remove-Item -LiteralPath $run -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "nagi-update-release: $script:pass passed, $script:fail failed"
if ($script:fail) { exit 1 }
exit 0
