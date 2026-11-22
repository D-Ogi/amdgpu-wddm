# Makes a custom kernel signers policy: a Code Integrity base policy that lets Windows load kernel drivers signed with
# one certificate while Secure Boot is on. Microsoft Learn, "Custom kernel signers for App Control for Business",
# https://learn.microsoft.com/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers
# (page of 2026-11-17), steps 4 to 7. Windows PowerShell 5.1 with the ConfigCI module (Windows 11 Pro and higher).
#
#   powershell -NoProfile -File New-CksPolicy.ps1 -UpdateSigner <kek.cer> -KernelSigner <driver.cer> -Out <empty dir>
#                                                [-PolicyName <name>]
#
# -UpdateSigner  the certificate that signs this policy and every later version of it. It must chain to a certificate
#                in the Secure Boot PK or KEK of the PC, or Windows refuses the policy.
# -KernelSigner  the certificate of the driver: for the amdgpu-wddm package, payload\cert\amdgpu-wddm-release.cer.
# -Out           a folder that does not exist or is empty.
#
# The output: CustomKernelSignersPolicy.xml, the unsigned binary policy <PolicyID>.cip and SIGN.txt, the signtool line
# for the owner of the update signer key. This tool reads public certificates only and never handles a private key.
# After the signature: copy the signed file to \EFI\Microsoft\Boot\CiPolicies\Active\<PolicyID>.cip on the EFI system
# partition, then reset Windows. Windows reads a new policy of this kind only in the first start after a reset or an
# install (docs/testing/INSTALL.md, section Secure Boot).
#
# The edits of the Microsoft template DefaultWindows_Enforced.xml, as tested in a VM on Windows 11 26300 (2026-11-22):
#   - Options 6 (unsigned policy), 0 (user mode code integrity), 2 (WHQL only) and 17 (supplemental policies) go.
#     ConvertFrom-CIPolicy refuses a signed base policy that allows supplemental policies without SupplementalSigners.
#   - The user mode scenario 12 goes as the last edit, because Add-SignerRule and Set-CIPolicyIdInfo write it back.
#     Without it the policy does not touch user mode programs.
param(
    [Parameter(Mandatory)][string]$UpdateSigner,
    [Parameter(Mandatory)][string]$KernelSigner,
    [Parameter(Mandatory)][string]$Out,
    [string]$PolicyName = 'amdgpu-wddm custom kernel signers'
)
$ErrorActionPreference = 'Stop'
$CodeSigningOid = '1.3.6.1.5.5.7.3.3'
$PolicyContentOid = '1.3.6.1.4.1.311.79.1'

function Read-Certificate([string]$Path, [string]$Role) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "the $Role certificate $Path does not exist" }
    try { return New-Object Security.Cryptography.X509Certificates.X509Certificate2 (, [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)) }
    catch { throw "the $Role certificate $Path is not an X.509 certificate: $($_.Exception.Message)" }
}
$update = Read-Certificate $UpdateSigner 'update signer'
$kernel = Read-Certificate $KernelSigner 'kernel signer'
if ($update.HasPrivateKey -or $kernel.HasPrivateKey) { throw 'give the public certificates (.cer), not a file with a private key' }
$eku = @($kernel.Extensions | Where-Object { $_ -is [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] })
if ($eku.Count -and -not @($eku[0].EnhancedKeyUsages | Where-Object { $_.Value -eq $CodeSigningOid }).Count) {
    throw "the kernel signer certificate $KernelSigner has no code signing use ($CodeSigningOid): Windows does not accept a driver signed with it"
}
if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "the output folder $Out is not empty" }
$template = Join-Path $env:WINDIR 'schemas\CodeIntegrity\ExamplePolicies\DefaultWindows_Enforced.xml'
if (-not (Test-Path -LiteralPath $template)) { throw "the Microsoft template $template is not on this Windows" }
Import-Module ConfigCI

[void][IO.Directory]::CreateDirectory($Out)
$OutFull = (Resolve-Path -LiteralPath $Out).Path
$xmlPath = Join-Path $OutFull 'CustomKernelSignersPolicy.xml'
Copy-Item -LiteralPath $template -Destination $xmlPath
function Read-Policy { [xml](Get-Content -LiteralPath $xmlPath -Raw) }
function Get-SignerTbs { @((Read-Policy).SiPolicy.Signers.Signer | ForEach-Object { [string]$_.CertRoot.Value }) }

foreach ($o in 6, 0, 2, 17) { Set-RuleOption -FilePath $xmlPath -Option $o -Delete }
$before = Get-SignerTbs
Add-SignerRule -FilePath $xmlPath -CertificatePath $UpdateSigner -Update
$updateTbs = @(Get-SignerTbs | Where-Object { $before -notcontains $_ })
$before = Get-SignerTbs
Add-SignerRule -FilePath $xmlPath -CertificatePath $KernelSigner -Kernel
$kernelTbs = @(Get-SignerTbs | Where-Object { $before -notcontains $_ })
if ($updateTbs.Count -ne 1 -or $kernelTbs.Count -ne 1) { throw "Add-SignerRule added $($updateTbs.Count) update and $($kernelTbs.Count) kernel signer(s), one of each expected" }
# -WarningAction: the cmdlets warn that a base policy without supplemental signers takes no supplemental policy, which
# is the intent here (option 17 is out).
Set-CIPolicyIdInfo -FilePath $xmlPath -ResetPolicyID -WarningAction SilentlyContinue | Out-Null
Set-CIPolicyIdInfo -FilePath $xmlPath -PolicyName $PolicyName -WarningAction SilentlyContinue | Out-Null
$x = Read-Policy
$umci = @($x.SiPolicy.SigningScenarios.SigningScenario | Where-Object { $_.Value -eq '12' })
foreach ($s in $umci) { [void]$x.SiPolicy.SigningScenarios.RemoveChild($s) }
$x.Save($xmlPath)

# Read back what Windows will get.
$x = Read-Policy
$options = @($x.SiPolicy.Rules.Rule | ForEach-Object { [string]$_.Option })
$left = @($options | Where-Object { $_ -in 'Enabled:UMCI', 'Required:WHQL', 'Enabled:Unsigned System Integrity Policy', 'Enabled:Allow Supplemental Policies' })
if ($left.Count) { throw "the policy still has the option(s) $($left -join ', ')" }
if (@($x.SiPolicy.SigningScenarios.SigningScenario | Where-Object { $_.Value -eq '12' }).Count) { throw 'the policy still has the user mode scenario 12' }
$id = [string]$x.SiPolicy.PolicyID
$cip = Join-Path $OutFull "$id.cip"
ConvertFrom-CIPolicy -XmlFilePath $xmlPath -BinaryFilePath $cip -WarningAction SilentlyContinue | Out-Null

$sign = @"
Sign $id.cip with the private key of this certificate (the update signer of the policy):
  $($update.Subject), thumbprint $($update.Thumbprint)
It must chain to a certificate in the Secure Boot PK or KEK of the PC.

  signtool sign /fd sha256 /p7 . /p7co $PolicyContentOid /p7ce Embedded /f <update signer .pfx> "$id.cip"

signtool writes $id.cip.p7. Copy it to \EFI\Microsoft\Boot\CiPolicies\Active\$id.cip on the EFI system partition
(mountvol S: /s), then reset Windows. A normal restart is not sufficient.
Kernel signer of the policy: $($kernel.Subject), thumbprint $($kernel.Thumbprint).
"@
[IO.File]::WriteAllText((Join-Path $OutFull 'SIGN.txt'), $sign.Replace("`r`n", "`n").Replace("`n", "`r`n"))
[pscustomobject]@{ policy_id = $id; xml = $xmlPath; cip = $cip; update_signer_tbs = $updateTbs[0]; kernel_signer_tbs = $kernelTbs[0]
    options = $options; scenarios = @($x.SiPolicy.SigningScenarios.SigningScenario | ForEach-Object { [string]$_.Value }) }
