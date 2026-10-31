# Host test of the user-mode signing route of a release (BD-160): tools\release\user-mode-signing.ps1 and the places
# where build-release.ps1 uses it. No certificate store is opened and nothing is signed: the store, the signatures
# and the files are given to the functions as script blocks and a folder under -WorkRoot. The real signing of a
# single file with signtool is in test-setup-exe.ps1.
#
#   powershell -File tools\release\test-user-mode-signing.ps1 -WorkRoot <empty or absent folder>
#
# Exit 0 = every check passed.
param([Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'user-mode-signing.ps1')
$script:pass = 0; $script:fail = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { $script:pass++; Write-Host "PASS $What" } else { $script:fail++; Write-Host "FAIL $What" }
}
function Refuses([scriptblock]$Block, [string]$Pattern, [string]$What) {
    $m = $null
    try { & $Block | Out-Null } catch { $m = $_.Exception.Message }
    Check ($null -ne $m -and $m -match $Pattern) "$What (refused: $m)"
}

$test = 'A' * 40
$ev = 'B' * 40
$now = [DateTime]::new(2026, 10, 31, 12, 0, 0, [DateTimeKind]::Utc)
function New-FakeCert([string]$Thumb, [bool]$Key = $true, [string[]]$Ekus = @('1.3.6.1.5.5.7.3.3'), [int]$DaysLeft = 300, [string]$Store = 'CurrentUser') {
    [pscustomobject]@{ Thumbprint = $Thumb; HasPrivateKey = $Key; Subject = 'CN=Example Publisher, O=Example'
        NotBefore = $now.AddDays(-30); NotAfter = $now.AddDays($DaysLeft); store = $Store
        EnhancedKeyUsageList = @($Ekus | ForEach-Object { [pscustomobject]@{ ObjectId = $_ } }) }
}
function Signer([string]$Thumb, [string]$Url, [string]$Flavour = 'release', $Cert) {
    $c = $Cert
    Get-UserModeSigner -Thumbprint $Thumb -TimestampUrl $Url -Flavour $Flavour -TestThumbprint $test -Now $now -FindCertificate { param($t) $c }.GetNewClosure()
}
$url = 'http://time.example.test/rfc3161'

# 1. The choice of signer.
$s = Signer '' '' 'release' $null
Check ($s.mode -eq 'test' -and $s.thumbprint -eq $test -and -not $s.timestamp_url) 'no thumbprint: the test certificate, no time stamp'
$s = Signer '' '' 'lab' $null
Check ($s.mode -eq 'test') 'a lab package is test-signed'
$s = Signer $ev.ToLowerInvariant() $url 'release' (New-FakeCert $ev)
Check ($s.mode -eq 'store' -and $s.thumbprint -eq $ev -and $s.store -eq 'CurrentUser' -and $s.timestamp_url -eq $url) `
    "a store certificate: mode store, thumbprint in capitals, CurrentUser, time stamp ($($s.mode) $($s.thumbprint) $($s.store) $($s.timestamp_url))"
Refuses { Signer '' $url 'release' $null } 'needs -UserModeSignerThumbprint' 'a time stamp server without a thumbprint'
Refuses { Signer $ev $url 'lab' (New-FakeCert $ev) } 'release flavour only' 'a publisher certificate on a lab package'
Refuses { Signer $ev '' 'release' (New-FakeCert $ev) } 'needs -TimestampUrl' 'a publisher certificate without a time stamp server'
Refuses { Signer $ev 'ftp://time.example.test' 'release' (New-FakeCert $ev) } 'http or https' 'a time stamp server that is not http or https'
Refuses { Signer $ev 'time.example.test' 'release' (New-FakeCert $ev) } 'http or https' 'a time stamp server that is not an address'
Refuses { Signer 'XYZ' $url 'release' (New-FakeCert $ev) } '40 hexadecimal' 'a thumbprint that is not 40 hexadecimal digits'
Refuses { Signer $test $url 'release' (New-FakeCert $test) } 'names the test certificate' 'the test certificate named as the publisher certificate'
Refuses { Signer $ev $url 'release' $null } 'no certificate' 'a certificate that is not in the store'
Refuses { Signer $ev $url 'release' (New-FakeCert $ev -Key $false) } 'no private key' 'a certificate without its private key'
Refuses { Signer $ev $url 'release' (New-FakeCert $ev -Ekus @('1.3.6.1.5.5.7.3.1')) } 'not for code signing' 'a certificate that is not for code signing'
Refuses { Signer $ev $url 'release' (New-FakeCert $ev -DaysLeft -1) } 'not valid now' 'an expired certificate'
$s = Signer $ev $url 'release' (New-FakeCert $ev -Store 'LocalMachine')
Check ($s.store -eq 'LocalMachine') 'a certificate of LocalMachine\My is found there'

# 2. The signtool arguments.
$testSigner = Signer '' '' 'release' $null
$a = Get-SignToolSignArguments -Signer $testSigner -Path 'X:\f.exe' -Pfx 'X:\k.pfx' -Password 'pw'
Check (($a -join ' ') -eq 'sign /fd SHA256 /f X:\k.pfx /p pw X:\f.exe') "the test route: $($a -join ' ')"
Refuses { Get-SignToolSignArguments -Signer $testSigner -Path 'X:\f.exe' } 'PFX' 'the test route without its PFX'
$storeSigner = Signer $ev $url 'release' (New-FakeCert $ev)
$a = Get-SignToolSignArguments -Signer $storeSigner -Path 'X:\f.exe' -Pfx 'X:\k.pfx' -Password 'pw'
Check (($a -join ' ') -eq "sign /fd SHA256 /sha1 $ev /s My /tr $url /td SHA256 X:\f.exe") "the store route: $($a -join ' ')"
Check (($a -notcontains '/p') -and ($a -notcontains 'pw') -and ($a -notcontains '/f')) 'the store route passes no PFX and no password'
$machine = Signer $ev $url 'release' (New-FakeCert $ev -Store 'LocalMachine')
$a = Get-SignToolSignArguments -Signer $machine -Path 'X:\f.exe'
Check (($a -join ' ') -eq "sign /fd SHA256 /sha1 $ev /s My /sm /tr $url /td SHA256 X:\f.exe") "the store route on LocalMachine: $($a -join ' ')"

# 3. The read-back of a signature.
function Sig([string]$Status, [string]$Thumb, [bool]$Stamp) {
    $o = [pscustomobject]@{ Status = $Status; SignerCertificate = $null; TimeStamperCertificate = $null }
    if ($Thumb) { $o.SignerCertificate = [pscustomobject]@{ Thumbprint = $Thumb } }
    if ($Stamp) { $o.TimeStamperCertificate = [pscustomobject]@{ Thumbprint = 'C' * 40 } }
    { param($p) $o }.GetNewClosure()
}
$ok = $true
try { Test-UserModeSignature -Signer $storeSigner -Path 'X:\f.exe' -ReadSignature (Sig 'Valid' $ev $true) } catch { $ok = $false }
Check $ok 'the store route accepts a valid, time-stamped signature of its certificate'
Refuses { Test-UserModeSignature -Signer $storeSigner -Path 'X:\f.exe' -ReadSignature (Sig 'Valid' $test $true) } 'is not signed by' 'the store route refuses another signer'
Refuses { Test-UserModeSignature -Signer $storeSigner -Path 'X:\f.exe' -ReadSignature (Sig 'Valid' $ev $false) } 'no time stamp' 'the store route refuses a signature without a time stamp'
Refuses { Test-UserModeSignature -Signer $storeSigner -Path 'X:\f.exe' -ReadSignature (Sig 'UnknownError' $ev $true) } 'not valid' 'the store route refuses a signature Windows does not trust'
Refuses { Test-UserModeSignature -Signer $storeSigner -Path 'X:\f.exe' -ReadSignature (Sig 'NotSigned' '' $false) } 'is not signed by' 'the store route refuses an unsigned file'
$ok = $true
try { Test-UserModeSignature -Signer $testSigner -Path 'X:\f.exe' -ReadSignature (Sig 'UnknownError' $test $false) } catch { $ok = $false }
Check $ok 'the test route accepts its certificate under a root this computer does not trust'
Refuses { Test-UserModeSignature -Signer $testSigner -Path 'X:\f.exe' -ReadSignature (Sig 'HashMismatch' $test $false) } 'not usable' 'the test route refuses a changed file'
Refuses { Test-UserModeSignature -Signer $testSigner -Path 'X:\f.exe' -ReadSignature (Sig 'Valid' $ev $true) } 'is not signed by' 'the test route refuses another signer'

# 4. Which files get which signer.
if (Test-Path -LiteralPath $WorkRoot) {
    if (@(Get-ChildItem -LiteralPath $WorkRoot -Force).Count) { throw "-WorkRoot $WorkRoot is not empty" }
} else { [void](New-Item -ItemType Directory -Path $WorkRoot) }
$top = Join-Path $WorkRoot 'pkg'
$kmd = Join-Path $top 'files\payload\kmd'
foreach ($p in 'files\payload\kmd\bc250kmd.sys', 'files\payload\kmd\d3d12\umd.dll', 'files\payload\kmd\d3d12\signed.dll',
        'files\payload\tools\cli.exe', 'files\payload\tools\readme.txt', 'files\payload\app\vendor.dll', 'setup.exe',
        'files\payload\kmdx\near.dll') {
    $f = Join-Path $top $p
    [void][IO.Directory]::CreateDirectory((Split-Path $f))
    [IO.File]::WriteAllText($f, 'x')
}
$signed = @('signed.dll', 'vendor.dll')
$split = Split-UnsignedPeFiles -PackageRoot $top -DriverDirectory $kmd -IsSigned { param($p) $signed -contains (Split-Path $p -Leaf) }.GetNewClosure()
$names = { param($l) (@($l | ForEach-Object { $_.Substring($top.Length + 1) }) | Sort-Object) -join ',' }
Check ((& $names $split.driver) -eq 'files\payload\kmd\bc250kmd.sys,files\payload\kmd\d3d12\umd.dll') "driver package files: $(& $names $split.driver)"
Check ((& $names $split.user) -eq 'files\payload\kmdx\near.dll,files\payload\tools\cli.exe,setup.exe') "files outside the driver package: $(& $names $split.user)"
Remove-Item -LiteralPath $WorkRoot -Recurse -Force

# 5. build-release.ps1 uses the route where it signs.
$br = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build-release.ps1'))
function Pos([string]$Pattern) { $m = [regex]::Match($br, $Pattern); if ($m.Success) { $m.Index } else { -1 } }
Check ((Pos '\[string\]\$UserModeSignerThumbprint') -gt 0 -and (Pos '\[string\]\$TimestampUrl') -gt 0) 'build-release takes -UserModeSignerThumbprint and -TimestampUrl'
$choose = Pos 'Get-UserModeSigner -Thumbprint \$UserModeSignerThumbprint -TimestampUrl \$TimestampUrl -Flavour \$Flavour'
Check ($choose -gt 0 -and $choose -lt (Pos "Write-Host 'commits'")) 'the signer is chosen before the build starts'
Check ((Pos 'Split-UnsignedPeFiles -PackageRoot \$pkgRoot -DriverDirectory \$kmd') -gt 0 -and
       (Pos 'foreach \(\$f in \$toSign\.driver\) \{ Invoke-Sign \$f \}') -gt 0 -and
       (Pos 'foreach \(\$f in \$toSign\.user\) \{ Invoke-UserModeSign \$f \}') -gt 0) 'driver package files take the test certificate, the others the user-mode signer'
Check ((Pos 'Get-SignToolSignArguments -Signer \$userSigner') -gt 0 -and (Pos 'Test-UserModeSignature -Signer \$userSigner -Path \$Path') -gt 0) 'every user-mode signature is read back at once'
$cab = Pos 'New-DriverSubmissionCab -DriverDirectory'
$cabSign = Pos "if \(\`$userSigner\.mode -eq 'store'\) \{\s*Invoke-UserModeSign \`$SubmissionCab"
Check ($cab -gt 0 -and $cabSign -gt $cab) 'the store route signs the submission CAB after it is written'
$one = Pos "make-setup-exe\.ps1'\) -PackageTop \`$pkgRoot -Out \`$setupOne -Check -ReleaseZip"
$oneSign = Pos 'Invoke-UserModeSign \$setupOne'
$oneCheck = Pos "make-setup-exe\.ps1'\) -PackageTop \`$pkgRoot -Out \`$setupOne -CheckSigned -ReleaseZip \`$zip"
$passGone = Pos '(?m)^\$pass = \$null'
$oneHash = Pos "'single exe \{0\} SHA256"
Check ($one -gt 0 -and $oneSign -gt $one -and $oneCheck -gt $oneSign -and $passGone -gt $oneCheck -and $oneHash -gt $passGone) `
    'the single file is written, signed, checked signed, and only then hashed; the password goes after its signature'
Check (([regex]::Matches($br, '(?m)^\$pass = \$null')).Count -eq 1) 'the password is cleared once, at the end'
Check ((Pos 'Test-DriverFileSignature -Path \$setupOne -SignTool \$signtool -ExpectedRoot \$ExpectedSigningRoot') -gt $oneCheck) 'a test-signed single file gets the pinned digest check'
Check ((Pos 'user_mode_signing = \[ordered\]@\{ mode = \$userSigner\.mode') -gt 0) 'manifest.json records the user-mode signer'

Write-Host "$($script:pass) PASS, $($script:fail) FAIL"
if ($script:fail) { exit 1 }
