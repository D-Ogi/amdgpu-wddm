# The H.264 encoder Media Foundation transform of the package (M15.11): the file the installer puts on the computer
# and the registry it writes, so that Game Bar, Windows Camera and Chromium find the transform.
# driver/umd/mft-h264/INSTALL.md is the specification. This is its route A, machine wide, which is the shape
# MFTRegister itself writes and the shape a shipping display driver package on the development PC uses (measured
# 2026-10-05). Route B (an MFT value in the display adapter's software key) is withdrawn there and is not written.
#
# Windows PowerShell 5.1 syntax only: install.ps1 and uninstall.ps1 dot-source this file.
#
# Two points differ from INSTALL.md on purpose:
#  - the DLL goes to <install root>\mft\, not to %SystemRoot%\System32 (DIRID 11). COM activates the transform
#    through the full path in InprocServer32, so System32 gives nothing, and the install root keeps the uninstall
#    complete: the whole tree goes at once and no file of ours stays behind in System32. The install root takes the
#    read rights of %ProgramFiles%, which every recording process needs, an AppContainer client included.
#  - nothing is written in the WOW6432Node view. The DLL is x64, so a 32-bit Media Foundation client could not load
#    it, and a 32-bit registration would only offer it one.
#
# The binary values are never written by hand. The shipped DLL exports Bc250BuildMftRegistration (mft_register.h),
# which is the code the live transform uses for its own enumeration view, so the registry and the object a client
# gets cannot drift apart. mftreg.exe --reg prints the same bytes.

$script:MftClsid         = '{A32438F0-0D79-4CA9-A5BF-9F3C80837253}'
# Under MediaFoundation\Transforms the class id and the category carry no braces: all 58 class id keys and all 9
# category keys of the development PC are written that way, the three of the NVIDIA display driver package included
# (INSTALL.md, step 2). The COM key keeps its braces, because SOFTWARE\Classes\CLSID is written that way.
$script:MftClsidBare     = 'A32438F0-0D79-4CA9-A5BF-9F3C80837253'
$script:MftCategoryBare  = 'F79EAC7D-E545-4387-BDEE-D647D7BDE42A'    # MFT_CATEGORY_VIDEO_ENCODER
$script:MftFriendlyName  = 'BC-250 H.264 Encoder MFT'
$script:MftFileName      = 'amdgpu_wddm_mft_h264.dll'
$script:MftPackagePath   = 'payload/mft/amdgpu_wddm_mft_h264.dll'
$script:ClassesKey       = 'HKLM:\SOFTWARE\Classes'   # host tests pass their own scratch key to every function here
$script:MftInteropDll    = $null                      # the DLL this process compiled its interop for
$script:MftValueNames    = @('MFTFlags', 'InputTypes', 'OutputTypes', 'Attributes')

# The five keys of the registration, in the order they are written. Removal goes the other way.
function Get-MftKeys {
    param([Parameter(Mandatory)][string]$ClassesKey)
    $com = "$ClassesKey\CLSID\$($script:MftClsid)"
    $transforms = "$ClassesKey\MediaFoundation\Transforms"
    $category = "$transforms\Categories\$($script:MftCategoryBare)"
    return [ordered]@{
        com        = $com
        inproc     = "$com\InprocServer32"
        transform  = "$transforms\$($script:MftClsidBare)"
        category   = $category
        membership = "$category\$($script:MftClsidBare)"
    }
}

# Which of our keys are on the computer now (the shared Categories key and MediaFoundation\Transforms itself are
# Windows' own and are never counted).
function Get-MftRegistrationKeysPresent {
    param([Parameter(Mandatory)][string]$ClassesKey)
    $k = Get-MftKeys $ClassesKey
    $present = New-Object System.Collections.ArrayList
    foreach ($p in @($k.com, $k.transform, $k.membership)) { if (Test-Path -LiteralPath $p) { [void]$present.Add($p) } }
    return @($present)
}

# What the release says about the encoder: manifest.json "mft_h264" (build-release.ps1 copies it from
# release-sources.json). A package without the section registers nothing, which is what every release up to
# tester.11 did.
function Get-MftReleaseSwitch {
    param($Manifest, [string]$PackageRoot)
    $s = $null
    if ($Manifest -and $Manifest.PSObject.Properties['mft_h264']) { $s = $Manifest.mft_h264 }
    $register = $false
    if ($s -and $s.register) { $register = $true }
    $rel = $script:MftPackagePath
    if ($s -and $s.package_path) { $rel = [string]$s.package_path }
    if ($rel -notmatch '^payload/[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+\.dll$') { throw "mft_h264 package_path '$rel': expected payload/<directory>/<name>.dll" }
    $source = $null
    $present = $false
    if ($PackageRoot) {
        $source = Join-Path $PackageRoot ($rel -replace '/', '\')
        $present = Test-Path -LiteralPath $source -PathType Leaf
    }
    return [ordered]@{
        register     = $register
        package_path = $rel
        payload_dir  = ($rel -split '/')[1]                             # the payload directory install.ps1 copies
        install_path = ($rel -replace '^payload/', '') -replace '/', '\' # under the install root
        source       = $source
        present      = $present
        name         = $script:MftFriendlyName
        clsid        = $script:MftClsid
    }
}

# What a run has to do about the encoder. Pure: the switch and what is on the computer in, the action out.
#   register   the release registers it: the DLL and the five keys go on
#   rollback   the release does not register it, and an earlier install left keys or files: they go
#   none       the release does not register it and there is nothing to take away
function Get-MftAction {
    param([Parameter(Mandatory)]$Switch, [AllowEmptyCollection()][string[]]$PresentKeys = @(), [bool]$DirPresent = $false)
    if ($Switch.register) { return 'register' }
    if (@($PresentKeys).Count -or $DirPresent) { return 'rollback' }
    return 'none'
}

# The C# interop for the shipped DLL. DllImport takes the full path, so the process loads the package's own copy and
# nothing from the search path. MFStartup comes first: the attribute blob is built with MFCreateAttributes.
function Get-MftInteropSource {
    param([Parameter(Mandatory)][ValidatePattern('^[A-Za-z]:\\[^"]+\.dll$')][string]$DllPath)
    return @"
using System;
using System.Runtime.InteropServices;
namespace Bc250MftRegistration {
 public static class Blob {
  [DllImport("mfplat.dll", ExactSpelling = true)]
  public static extern int MFStartup(uint version, uint flags);
  [DllImport("mfplat.dll", ExactSpelling = true)]
  public static extern int MFShutdown();
  [DllImport(@"$DllPath", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
  public static extern int Bc250BuildMftRegistration(uint which, byte[] buffer, uint capacity, out uint needed);
 }
}
"@
}

# The four registry values, read out of the shipped DLL: MFTFlags as a number, the other three as bytes.
# RegBlob of mft_register.h: 0 Attributes, 1 InputTypes, 2 OutputTypes, 3 MftFlags.
function Get-MftRegistrationBlobs {
    param([Parameter(Mandatory)][string]$DllPath)
    if (-not (Test-Path -LiteralPath $DllPath -PathType Leaf)) { throw "no $DllPath (the H.264 encoder MFT of this package)" }
    $full = (Get-Item -LiteralPath $DllPath).FullName
    if (-not ('Bc250MftRegistration.Blob' -as [type])) {
        Add-Type -TypeDefinition (Get-MftInteropSource -DllPath $full)
        $script:MftInteropDll = $full
    } elseif ($script:MftInteropDll -ne $full) {
        # One process can bind the import to one path only.
        throw "the registration interop of this process is bound to $($script:MftInteropDll); $full needs its own process"
    }
    $hr = [Bc250MftRegistration.Blob]::MFStartup(0x00020070, 1)    # MF_VERSION, MFSTARTUP_LITE
    if ($hr -lt 0) { throw ('MFStartup failed 0x{0:X8}' -f $hr) }
    try {
        $raw = @{}
        foreach ($b in @(@{ name = 'Attributes'; which = 0 }, @{ name = 'InputTypes'; which = 1 }, @{ name = 'OutputTypes'; which = 2 }, @{ name = 'MftFlags'; which = 3 })) {
            $need = 0
            # The first call asks for the size (ERROR_INSUFFICIENT_BUFFER), the second reads the bytes.
            [void][Bc250MftRegistration.Blob]::Bc250BuildMftRegistration([uint32]$b.which, $null, 0, [ref]$need)
            if ($need -eq 0) { throw "$($b.name): the DLL reports an empty registration value" }
            $buf = New-Object byte[] $need
            $hr = [Bc250MftRegistration.Blob]::Bc250BuildMftRegistration([uint32]$b.which, $buf, [uint32]$need, [ref]$need)
            if ($hr -lt 0) { throw ('Bc250BuildMftRegistration({0}) failed 0x{1:X8}' -f $b.which, $hr) }
            $raw[$b.name] = $buf
        }
    } finally { [void][Bc250MftRegistration.Blob]::MFShutdown() }
    if ($raw['MftFlags'].Length -ne 4) { throw "MFTFlags: $($raw['MftFlags'].Length) bytes, expected 4" }
    $flags = [BitConverter]::ToUInt32($raw['MftFlags'], 0)
    # MFT_ENUM_FLAG_HARDWARE (0x4) is what makes a client that asks for a hardware encoder see the transform at all.
    if (($flags -band 0x4) -eq 0) { throw ('MFTFlags 0x{0:X8} has no MFT_ENUM_FLAG_HARDWARE: a hardware encoder client would not see the transform' -f $flags) }
    # MFT_REGISTER_TYPE_INFO is two GUIDs: both type lists are whole pairs and neither is empty.
    foreach ($n in 'InputTypes', 'OutputTypes') {
        if ($raw[$n].Length -eq 0 -or ($raw[$n].Length % 32) -ne 0) { throw "${n}: $($raw[$n].Length) bytes, expected a non-zero multiple of 32" }
    }
    return [ordered]@{ MFTFlags = [int]$flags; InputTypes = [byte[]]$raw['InputTypes']; OutputTypes = [byte[]]$raw['OutputTypes']
        Attributes = [byte[]]$raw['Attributes']; dll = $full }
}

# One line for the log and for the dry run: every key and value the registration writes.
function Format-MftRegistration {
    param([Parameter(Mandatory)][string]$ClassesKey, [Parameter(Mandatory)][string]$DllPath, $Blobs)
    $k = Get-MftKeys $ClassesKey
    $sizes = ''
    if ($Blobs) { $sizes = (' MFTFlags 0x{0:X8}, InputTypes {1} bytes, OutputTypes {2}, Attributes {3};' -f $Blobs.MFTFlags, $Blobs.InputTypes.Length, $Blobs.OutputTypes.Length, $Blobs.Attributes.Length) }
    return ("$($k.com) = '$($script:MftFriendlyName)', InprocServer32 = $DllPath (ThreadingModel Both); " +
            "$($k.transform):$sizes $($k.membership) (the key is the category membership, it holds no value)")
}

# Writes the registration. Every value is overwritten, so a repair over an earlier install needs no removal first.
function Write-MftRegistration {
    param([Parameter(Mandatory)][string]$ClassesKey, [Parameter(Mandatory)][string]$DllPath, [Parameter(Mandatory)]$Blobs)
    $k = Get-MftKeys $ClassesKey
    Initialize-RegistryKey $k.com
    New-ItemProperty -LiteralPath $k.com -Name '(default)' -Value $script:MftFriendlyName -PropertyType String -Force | Out-Null
    Initialize-RegistryKey $k.inproc
    New-ItemProperty -LiteralPath $k.inproc -Name '(default)' -Value $DllPath -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k.inproc -Name 'ThreadingModel' -Value 'Both' -PropertyType String -Force | Out-Null
    Initialize-RegistryKey $k.transform
    New-ItemProperty -LiteralPath $k.transform -Name '(default)' -Value $script:MftFriendlyName -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k.transform -Name 'MFTFlags' -Value ([int]$Blobs.MFTFlags) -PropertyType DWord -Force | Out-Null
    foreach ($n in 'InputTypes', 'OutputTypes', 'Attributes') {
        New-ItemProperty -LiteralPath $k.transform -Name $n -Value ([byte[]]$Blobs.$n) -PropertyType Binary -Force | Out-Null
    }
    Initialize-RegistryKey $k.membership
    return @($k.com, $k.inproc, $k.transform, $k.membership)
}

# Takes the registration away: our class id key, our transform key and our membership in the category. Returns the
# keys it removed.
function Remove-MftRegistration {
    param([Parameter(Mandatory)][string]$ClassesKey)
    $k = Get-MftKeys $ClassesKey
    $removed = New-Object System.Collections.ArrayList
    foreach ($p in @($k.membership, $k.transform, $k.com)) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $p)) { [void]$removed.Add($p) }
    }
    # The category key belongs to every video encoder of the machine, Windows' own included: it goes only when our
    # membership was the last thing in it, and MediaFoundation\Transforms itself never goes.
    $cat = Get-Item -LiteralPath $k.category -ErrorAction SilentlyContinue
    if ($cat -and $cat.SubKeyCount -eq 0 -and $cat.ValueCount -eq 0) {
        Remove-Item -LiteralPath $k.category -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path -LiteralPath $k.category)) { [void]$removed.Add($k.category) }
    }
    return @($removed)
}

# Is the registration on the computer, and does it say what this package says? Returns ok, the reasons it is not, and
# which keys are there. $Blobs and $DllPath are optional: without them only the keys and the names are judged.
function Test-MftRegistration {
    param([Parameter(Mandatory)][string]$ClassesKey, [string]$DllPath, $Blobs)
    $k = Get-MftKeys $ClassesKey
    $bad = New-Object System.Collections.ArrayList
    foreach ($p in @($k.com, $k.inproc, $k.transform, $k.membership)) { if (-not (Test-Path -LiteralPath $p)) { [void]$bad.Add("no $p") } }
    $com = Get-Item -LiteralPath $k.com -ErrorAction SilentlyContinue
    if ($com -and [string]$com.GetValue('') -ne $script:MftFriendlyName) { [void]$bad.Add("$($k.com) names '$($com.GetValue(''))'") }
    $inproc = Get-Item -LiteralPath $k.inproc -ErrorAction SilentlyContinue
    if ($inproc) {
        if ($DllPath -and [string]$inproc.GetValue('') -ne $DllPath) { [void]$bad.Add("InprocServer32 = '$($inproc.GetValue(''))', expected $DllPath") }
        if ([string]$inproc.GetValue('ThreadingModel') -ne 'Both') { [void]$bad.Add("ThreadingModel = '$($inproc.GetValue('ThreadingModel'))'") }
    }
    $t = Get-Item -LiteralPath $k.transform -ErrorAction SilentlyContinue
    if ($t) {
        if ([string]$t.GetValue('') -ne $script:MftFriendlyName) { [void]$bad.Add("$($k.transform) names '$($t.GetValue(''))'") }
        foreach ($n in $script:MftValueNames) { if ($null -eq $t.GetValue($n)) { [void]$bad.Add("$($k.transform): no $n") } }
        # A value that is missing was named above; GetValueKind throws on one, so each check needs the value first.
        if ($Blobs -and $null -ne $t.GetValue('MFTFlags')) {
            if ([int]$t.GetValue('MFTFlags') -ne [int]$Blobs.MFTFlags) { [void]$bad.Add("MFTFlags $($t.GetValue('MFTFlags')), expected $($Blobs.MFTFlags)") }
            if ($t.GetValueKind('MFTFlags') -ne 'DWord') { [void]$bad.Add("MFTFlags is $($t.GetValueKind('MFTFlags')), not REG_DWORD") }
        }
        if ($Blobs) {
            foreach ($n in 'InputTypes', 'OutputTypes', 'Attributes') {
                if ($null -eq $t.GetValue($n)) { continue }
                $have = [byte[]]$t.GetValue($n)
                if ($t.GetValueKind($n) -ne 'Binary') { [void]$bad.Add("$n is $($t.GetValueKind($n)), not REG_BINARY") }
                elseif (([BitConverter]::ToString($have)) -ne ([BitConverter]::ToString([byte[]]$Blobs.$n))) { [void]$bad.Add("$n differs from the shipped DLL ($($have.Length) bytes, expected $(([byte[]]$Blobs.$n).Length))") }
            }
        }
    }
    return [ordered]@{ ok = (@($bad).Count -eq 0); detail = (@($bad) -join '; '); keys = @(Get-MftRegistrationKeysPresent -ClassesKey $ClassesKey) }
}
