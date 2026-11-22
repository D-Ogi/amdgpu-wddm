# Host test of New-CksPolicy.ps1, Windows PowerShell 5.1 (the ConfigCI cmdlets):
#   powershell -NoProfile -File tools\win\cks-policy\test.ps1 [-WorkRoot <dir>]
# Two throwaway certificates are made in memory (no certificate store, no private key on disk). The tool must write a
# policy that names the first as the update signer and the second as a kernel signer, keep no user-mode scenario and
# none of the options 0, 2, 6 and 17, and write a binary policy. Negative controls: a missing certificate and a
# certificate without a code signing use are refused, and nothing is written for them.
param([string]$WorkRoot = (Join-Path $env:TEMP 'cks-policy-test'))
$ErrorActionPreference = 'Stop'
$tool = Join-Path $PSScriptRoot 'New-CksPolicy.ps1'
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
$work = Join-Path $WorkRoot ('run-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
[void][IO.Directory]::CreateDirectory($work)

function New-TestCertificate([string]$Subject, [string[]]$Eku) {
    $rsa = [Security.Cryptography.RSA]::Create(2048)
    $req = New-Object Security.Cryptography.X509Certificates.CertificateRequest($Subject, $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    if ($Eku) {
        $oids = New-Object Security.Cryptography.OidCollection
        foreach ($e in $Eku) { [void]$oids.Add((New-Object Security.Cryptography.Oid $e)) }
        $req.CertificateExtensions.Add((New-Object Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension($oids, $false)))
    }
    $c = $req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1), [DateTimeOffset]::UtcNow.AddDays(30))
    return $c.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
}
$codeSigning = '1.3.6.1.5.5.7.3.3'
$kek = Join-Path $work 'test-kek.cer'; [IO.File]::WriteAllBytes($kek, (New-TestCertificate 'CN=cks-policy test KEK' @()))
$drv = Join-Path $work 'test-driver.cer'; [IO.File]::WriteAllBytes($drv, (New-TestCertificate 'CN=cks-policy test driver' @($codeSigning)))
$noSign = Join-Path $work 'test-nosign.cer'; [IO.File]::WriteAllBytes($noSign, (New-TestCertificate 'CN=cks-policy test server' @('1.3.6.1.5.5.7.3.1')))

'[cks-policy] a policy for one driver certificate'
$out = Join-Path $work 'out'
$r = & $tool -UpdateSigner $kek -KernelSigner $drv -Out $out -PolicyName 'cks-policy test'
$xmlPath = Join-Path $out 'CustomKernelSignersPolicy.xml'
Check (Test-Path $xmlPath) 'the policy XML is written'
[xml]$x = Get-Content -LiteralPath $xmlPath
$id = [string]$x.SiPolicy.PolicyID
Check ($r.policy_id -eq $id -and $id -match '^\{[0-9A-F-]{36}\}$') "the result names the policy ID of the XML: $id"
$cip = Join-Path $out "$id.cip"
Check ((Test-Path $cip) -and ((Get-Item $cip).Length -gt 0) -and ($r.cip -eq $cip)) "the binary policy is written under its ID: $((Get-Item $cip).Length) bytes"
$options = @($x.SiPolicy.Rules.Rule | ForEach-Object { [string]$_.Option })
foreach ($o in 'Enabled:UMCI', 'Required:WHQL', 'Enabled:Unsigned System Integrity Policy', 'Enabled:Allow Supplemental Policies') {
    Check ($options -notcontains $o) "option '$o' is not in the policy"
}
$scenarios = @($x.SiPolicy.SigningScenarios.SigningScenario | ForEach-Object { [string]$_.Value })
Check (($scenarios -contains '131') -and ($scenarios -notcontains '12')) "the kernel scenario 131 only: $($scenarios -join ',')"
$signers = @{}; foreach ($s in $x.SiPolicy.Signers.Signer) { $signers[[string]$s.ID] = $s }
$kekTbs = $r.update_signer_tbs
$updateIds = @($x.SiPolicy.UpdatePolicySigners.UpdatePolicySigner | ForEach-Object { [string]$_.SignerId })
Check (@($updateIds | Where-Object { $signers[$_].CertRoot.Value -eq $kekTbs }).Count -eq 1) "the update signer is the KEK certificate (TBS $kekTbs)"
$kernel = $x.SiPolicy.SigningScenarios.SigningScenario | Where-Object { $_.Value -eq '131' }
$allowed = @($kernel.ProductSigners.AllowedSigners.AllowedSigner | ForEach-Object { [string]$_.SignerId })
Check (@($allowed | Where-Object { $signers[$_].CertRoot.Value -eq $r.kernel_signer_tbs }).Count -eq 1) "the kernel scenario allows the driver certificate (TBS $($r.kernel_signer_tbs))"
Check (([string]$x.SiPolicy.PolicyID) -eq ([string]$x.SiPolicy.BasePolicyID)) 'a base policy: PolicyID = BasePolicyID'
$signTxt = Join-Path $out 'SIGN.txt'
Check ((Test-Path $signTxt) -and ((Get-Content -Raw $signTxt) -match [regex]::Escape('/p7co 1.3.6.1.4.1.311.79.1')) -and ((Get-Content -Raw $signTxt) -match [regex]::Escape("$id.cip"))) 'SIGN.txt has the signtool line with the content OID and the file'
Check (@(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.pfx', '.key', '.pem', '.p12' }).Count -eq 0) 'no private key in the output'

'[cks-policy] refusals'
$bad = Join-Path $work 'bad1'
$e = $null; try { & $tool -UpdateSigner $kek -KernelSigner (Join-Path $work 'absent.cer') -Out $bad | Out-Null } catch { $e = $_.Exception.Message }
Check (($e -match 'absent\.cer') -and -not (Test-Path $bad)) "a missing certificate is refused, nothing written: $e"
$bad = Join-Path $work 'bad2'
$e = $null; try { & $tool -UpdateSigner $kek -KernelSigner $noSign -Out $bad | Out-Null } catch { $e = $_.Exception.Message }
Check (($e -match 'code signing') -and -not (Test-Path $bad)) "a driver certificate without the code signing use is refused: $e"
$e = $null; try { & $tool -UpdateSigner $kek -KernelSigner $drv -Out $out | Out-Null } catch { $e = $_.Exception.Message }
Check ($e -match 'not empty') "an output folder that is not empty is refused: $e"

if ($fail) { "$fail check(s) failed"; exit 1 } else { 'all checks passed' }
