# Host test of the H.264 encoder MFT registration (installer\mft-h264.ps1): install, repair and uninstall of the five
# keys of driver/umd/mft-h264/INSTALL.md route A, and the decision that the release switch drives. Windows PowerShell
# 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-mft-h264.ps1 [-Installer <package>\installer] [-Dll <mft dll>]
# Every write goes to a scratch key, HKCU:\Software\amdgpu-wddm-installer-test\Classes, removed at the end. The real
# HKLM:\SOFTWARE\Classes is only read, and the test fails if its MFT keys change while it runs.
# Without the DLL (an installer folder with no package next to it) the registry cases run against a made-up value set
# and the cases that read the shipped DLL are skipped.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'),
      [string]$Dll = (Join-Path (Split-Path $Installer) 'payload\mft\amdgpu_wddm_mft_h264.dll'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'mft-h264.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Get-Hex($Bytes) { if ($null -eq $Bytes) { return '' } return [BitConverter]::ToString([byte[]]$Bytes) }

# The real machine's keys, before anything: this test must leave them exactly as they are.
$realBefore = @(Get-MftRegistrationKeysPresent -ClassesKey 'HKLM:\SOFTWARE\Classes')
"real HKLM:\SOFTWARE\Classes: $(@($realBefore).Count) of our keys present (read only)"

$root = 'HKCU:\Software\amdgpu-wddm-installer-test'
$classes = "$root\Classes"
$keys = Get-MftKeys $classes
Check ($classes -like 'HKCU:*') "the test writes under $classes, never under HKLM"

'the release switch (manifest.json mft_h264)'
$sw = Get-MftReleaseSwitch -Manifest ([pscustomobject]@{ version = 'x' }) -PackageRoot $null
Check (-not $sw.register) 'a package without the section registers nothing'
$sw = Get-MftReleaseSwitch -Manifest ([pscustomobject]@{ mft_h264 = [pscustomobject]@{ register = $false } }) -PackageRoot $null
Check (-not $sw.register) 'register false registers nothing'
$sw = Get-MftReleaseSwitch -Manifest ([pscustomobject]@{ mft_h264 = [pscustomobject]@{ register = $true } }) -PackageRoot (Split-Path $Installer)
Check ($sw.register) 'register true registers'
Check ($sw.package_path -eq 'payload/mft/amdgpu_wddm_mft_h264.dll') "default package path $($sw.package_path)"
Check ($sw.payload_dir -eq 'mft') "payload directory $($sw.payload_dir)"
Check ($sw.install_path -eq 'mft\amdgpu_wddm_mft_h264.dll') "install path under the install root: $($sw.install_path)"
Check ($sw.clsid -eq '{A32438F0-0D79-4CA9-A5BF-9F3C80837253}') "class id $($sw.clsid) (INSTALL.md)"
Check ($sw.name -eq 'BC-250 H.264 Encoder MFT') "friendly name '$($sw.name)' (INSTALL.md)"
$threw = $false
try { [void](Get-MftReleaseSwitch -Manifest ([pscustomobject]@{ mft_h264 = [pscustomobject]@{ register = $true; package_path = 'payload/mft.dll' } }) -PackageRoot 'C:\x') } catch { $threw = $true }
Check $threw 'a package path outside payload/<directory>/<name>.dll is refused'

'the decision (Get-MftAction)'
Check ((Get-MftAction -Switch ([ordered]@{ register = $true }) -PresentKeys @() -DirPresent $false) -eq 'register') 'switch on, nothing there: register'
Check ((Get-MftAction -Switch ([ordered]@{ register = $true }) -PresentKeys @('k') -DirPresent $true) -eq 'register') 'switch on over an earlier install: register (every value is overwritten)'
Check ((Get-MftAction -Switch ([ordered]@{ register = $false }) -PresentKeys @() -DirPresent $false) -eq 'none') 'switch off, nothing there: none'
Check ((Get-MftAction -Switch ([ordered]@{ register = $false }) -PresentKeys @('k') -DirPresent $false) -eq 'rollback') 'switch off, keys of an earlier install: rollback'
Check ((Get-MftAction -Switch ([ordered]@{ register = $false }) -PresentKeys @() -DirPresent $true) -eq 'rollback') 'switch off, files of an earlier install: rollback'

'the four registry values out of the shipped DLL'
$blobs = $null
if (Test-Path -LiteralPath $Dll -PathType Leaf) {
    $blobs = Get-MftRegistrationBlobs -DllPath $Dll
    "  $Dll"
    # INSTALL.md publishes MFTFlags 0x00000006 = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT. A change of this
    # value has to be measured against mfthost --mft and the sink writer stage first (INSTALL.md, step 2).
    Check ($blobs.MFTFlags -eq 6) ('MFTFlags 0x{0:X8} (HARDWARE|ASYNCMFT, INSTALL.md)' -f $blobs.MFTFlags)
    Check (($blobs.InputTypes.Length % 32) -eq 0 -and $blobs.InputTypes.Length -ge 32) "InputTypes $($blobs.InputTypes.Length) bytes = $($blobs.InputTypes.Length / 32) type pair(s)"
    Check (($blobs.OutputTypes.Length % 32) -eq 0 -and $blobs.OutputTypes.Length -ge 32) "OutputTypes $($blobs.OutputTypes.Length) bytes = $($blobs.OutputTypes.Length / 32) type pair(s)"
    Check ($blobs.Attributes.Length -ge 8) "Attributes $($blobs.Attributes.Length) bytes (MFGetAttributesAsBlob)"
    # The same bytes twice: the values come from the DLL's own code, so two reads cannot differ.
    $again = Get-MftRegistrationBlobs -DllPath $Dll
    Check ((Get-Hex $again.Attributes) -eq (Get-Hex $blobs.Attributes) -and (Get-Hex $again.InputTypes) -eq (Get-Hex $blobs.InputTypes)) 'a second read of the DLL gives the same bytes'
} else {
    "  (the DLL cases are skipped, the registry cases use a made-up value set; no $($Dll))"
    $blobs = [ordered]@{ MFTFlags = 6; InputTypes = [byte[]](1..96 | ForEach-Object { $_ % 256 }); OutputTypes = [byte[]](1..32 | ForEach-Object { $_ % 256 })
        Attributes = [byte[]](1..276 | ForEach-Object { $_ % 256 }); dll = $null }
}

$dllTarget = 'C:\Program Files\amdgpu-wddm\mft\amdgpu_wddm_mft_h264.dll'
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
try {
    'install: the five keys of route A'
    # A video encoder of another vendor, already registered on this machine: nothing we do may touch it.
    $otherTransform = "$classes\MediaFoundation\Transforms\11111111-2222-3333-4444-555555555555"
    $otherMember = "$($keys.category)\11111111-2222-3333-4444-555555555555"
    Initialize-RegistryKey $otherTransform
    New-ItemProperty -LiteralPath $otherTransform -Name '(default)' -Value 'Another Encoder MFT' -PropertyType String -Force | Out-Null
    Initialize-RegistryKey $otherMember
    $written = @(Write-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs)
    Check (@($written).Count -eq 4) "$(@($written).Count) keys written"
    foreach ($p in @($keys.com, $keys.inproc, $keys.transform, $keys.category, $keys.membership)) { Check (Test-Path -LiteralPath $p) "key present: $($p.Substring($classes.Length + 1))" }
    $com = Get-Item -LiteralPath $keys.com
    Check ([string]$com.GetValue('') -eq 'BC-250 H.264 Encoder MFT') "CLSID default value '$($com.GetValue(''))'"
    $inproc = Get-Item -LiteralPath $keys.inproc
    Check ([string]$inproc.GetValue('') -eq $dllTarget) "InprocServer32 default value $($inproc.GetValue(''))"
    Check ($inproc.GetValueKind('') -eq 'String' -and [string]$inproc.GetValue('ThreadingModel') -eq 'Both') "ThreadingModel $($inproc.GetValue('ThreadingModel')), both values REG_SZ"
    $t = Get-Item -LiteralPath $keys.transform
    Check ([string]$t.GetValue('') -eq 'BC-250 H.264 Encoder MFT') "transform default value '$($t.GetValue(''))'"
    Check ($t.GetValueKind('MFTFlags') -eq 'DWord' -and [int]$t.GetValue('MFTFlags') -eq [int]$blobs.MFTFlags) "MFTFlags REG_DWORD $([int]$t.GetValue('MFTFlags'))"
    foreach ($n in 'InputTypes', 'OutputTypes', 'Attributes') {
        Check ($t.GetValueKind($n) -eq 'Binary' -and (Get-Hex $t.GetValue($n)) -eq (Get-Hex $blobs.$n)) "$n REG_BINARY, $(([byte[]]$t.GetValue($n)).Length) bytes as the DLL gives them"
    }
    $m = Get-Item -LiteralPath $keys.membership
    Check ($m.ValueCount -eq 0 -and $m.SubKeyCount -eq 0) 'the category membership key holds no value (the key is the membership)'
    Check (@(Get-MftRegistrationKeysPresent -ClassesKey $classes).Count -eq 3) 'three of our own keys are counted (the shared category key is not ours)'
    $v = Test-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs
    Check $v.ok "the registration verifies: $(if ($v.ok) { 'ok' } else { $v.detail })"

    'repair over a damaged registration'
    New-ItemProperty -LiteralPath $keys.inproc -Name 'ThreadingModel' -Value 'Apartment' -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $keys.inproc -Name '(default)' -Value 'C:\old\amdgpu_wddm_mft_h264.dll' -PropertyType String -Force | Out-Null
    Remove-ItemProperty -LiteralPath $keys.transform -Name 'Attributes' -Force
    New-ItemProperty -LiteralPath $keys.transform -Name 'MFTFlags' -Value 4 -PropertyType DWord -Force | Out-Null
    Remove-Item -LiteralPath $keys.membership -Recurse -Force
    $bad = Test-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs
    Check (-not $bad.ok) "the damaged registration fails the check: $($bad.detail)"
    [void](Write-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs)
    $v = Test-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs
    Check $v.ok "repair writes every value again: $(if ($v.ok) { 'ok' } else { $v.detail })"
    Check ([string](Get-Item -LiteralPath $keys.inproc).GetValue('') -eq $dllTarget) 'repair corrects the InprocServer32 path'
    Check (Test-Path -LiteralPath $keys.membership) 'repair creates the category membership again'

    'uninstall (and the rollback of a repair with the switch off)'
    Check ((Get-MftAction -Switch ([ordered]@{ register = $false }) -PresentKeys @(Get-MftRegistrationKeysPresent -ClassesKey $classes)) -eq 'rollback') 'switch off over this installation: rollback'
    $removed = @(Remove-MftRegistration -ClassesKey $classes)
    Check (@($removed).Count -eq 3) "$(@($removed).Count) keys removed"
    foreach ($p in @($keys.com, $keys.transform, $keys.membership)) { Check (-not (Test-Path -LiteralPath $p)) "key gone: $($p.Substring($classes.Length + 1))" }
    Check (-not (Test-Path -LiteralPath "$classes\CLSID\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}")) 'the class id key goes with its InprocServer32 subkey'
    Check ((Test-Path -LiteralPath $otherTransform) -and ([string](Get-Item -LiteralPath $otherTransform).GetValue('') -eq 'Another Encoder MFT')) 'the other vendor transform key is untouched'
    Check (Test-Path -LiteralPath $otherMember) 'the other vendor category membership is untouched'
    Check (Test-Path -LiteralPath $keys.category) 'the shared category key stays while another encoder is in it'
    Check (Test-Path -LiteralPath "$classes\MediaFoundation\Transforms") 'MediaFoundation\Transforms itself stays'
    Check ((Get-MftAction -Switch ([ordered]@{ register = $false }) -PresentKeys @(Get-MftRegistrationKeysPresent -ClassesKey $classes)) -eq 'none') 'after the rollback there is nothing left to do'
    $v = Test-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs
    Check (-not $v.ok -and @($v.keys).Count -eq 0) 'nothing of ours answers after the removal'
    [void](Remove-MftRegistration -ClassesKey $classes)
    Check (Test-Path -LiteralPath $keys.category) 'a second removal changes nothing'

    'the category key of a machine that has no other encoder'
    Remove-Item -LiteralPath $otherMember -Recurse -Force
    [void](Write-MftRegistration -ClassesKey $classes -DllPath $dllTarget -Blobs $blobs)
    [void](Remove-MftRegistration -ClassesKey $classes)
    Check (-not (Test-Path -LiteralPath $keys.category)) 'an empty category key goes with our membership'
    Check (Test-Path -LiteralPath "$classes\MediaFoundation\Transforms\Categories") 'the Categories key itself stays'
} finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $root)) 'scratch key removed'

'the installer and the uninstaller use these functions'
$inst = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
$un = [IO.File]::ReadAllText((Join-Path $Installer 'uninstall.ps1'))
Check ($inst -match 'Get-MftReleaseSwitch -Manifest \$script:Manifest -PackageRoot \$package') 'install.ps1 reads the switch from the manifest'
Check (($inst -match 'Write-MftRegistration -ClassesKey \$script:ClassesKey') -and ($inst -match 'Remove-MftRegistration -ClassesKey \$script:ClassesKey')) 'install.ps1 registers and rolls back'
Check ($inst.IndexOf('Write-MftRegistration') -gt $inst.IndexOf('$dirs += $mft.payload_dir')) 'install.ps1 copies the DLL before it writes the keys'
Check ($un -match 'Remove-MftRegistration -ClassesKey \$script:ClassesKey') 'uninstall.ps1 removes the keys'
foreach ($f in 'install.ps1', 'uninstall.ps1') {
    Check ([IO.File]::ReadAllText((Join-Path $Installer $f)) -match "\. \(Join-Path \`$here 'mft-h264\.ps1'\)") "$f dot-sources mft-h264.ps1"
}
$mod = [IO.File]::ReadAllText((Join-Path $Installer 'mft-h264.ps1'))
# The code without its comments: the comments name both of these to say why they are not used.
$modCode = (@($mod -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n")
Check ($modCode -notmatch 'regsvr32') 'nothing calls regsvr32 (the DLL exports no DllRegisterServer on purpose)'
Check ($modCode -notmatch 'WOW6432Node') 'nothing is written in the WOW6432Node view (the DLL is x64)'
Check (-not ($mod.ToCharArray() | Where-Object { [int]$_ -gt 127 } | Select-Object -First 1)) 'mft-h264.ps1 is ASCII'

$realAfter = @(Get-MftRegistrationKeysPresent -ClassesKey 'HKLM:\SOFTWARE\Classes')
Check ((@($realBefore) -join ',') -eq (@($realAfter) -join ',')) "the real HKLM:\SOFTWARE\Classes is as before ($(@($realAfter).Count) of our keys)"
if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'H.264 encoder MFT registration: all checks passed'
exit 0
