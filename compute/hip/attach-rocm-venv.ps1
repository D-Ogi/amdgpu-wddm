# attach-rocm-venv.ps1 - puts our two ROCm-facing DLLs into a TheRock Python environment, and takes them out.
#
# A TheRock venv (rocm_sdk_core, rocm_sdk_libraries, torch) loads AMD's HIP runtime and AMD's hipBLASLt.
# On this device both need ours in their place:
#   - amdhip64_7.dll, the AMD ABI face of our HIP runtime (build-amd-abi.ps1), in _rocm_sdk_core\bin. AMD's
#     DLL is kept beside it as amdhip64_7.dll.orig and is never loaded (D015: no second HIP runtime).
#   - libhipblaslt.dll, our hipBLASLt front (build-blaslt-front.ps1), in _rocm_sdk_libraries\bin. AMD's DLL
#     becomes libhipblaslt_amd.dll in the same directory; the front forwards to it by that name, and AMD's
#     kpack device code still resolves from that directory.
# The payload directory (-Payload, by default the directory of this script; the installer puts it in
# <install root>\tools\hip\rocm) holds the two DLLs, this script and payload.json (make-rocm-payload.ps1):
#   { "schema": 2, "therock": "<version>", "face": { "file", "amd_sha256", "build_sha256" },
#     "blaslt_front": { "file", "amd_sha256", "build_sha256", "amd_exports", "served_names" } }
# amd_sha256 names the AMD DLL each one was built and tested for. A venv whose AMD DLL is another one (another
# TheRock nightly, or a pip upgrade) is refused: the front's export table is AMD's table of that build. Our two
# DLLs are the files of the payload directory as they are now: the release stamps a version resource on each and
# signs it, so their bytes are not the build_sha256 of the build. The package manifest checks them. A payload.json
# of schema 1 (the kits up to kit-int8-1) gives "sha256" for each, and then the file must have it.
#
# Modes:
#   (none)    attach: replace both AMD DLLs; a step already in place is skipped. Writes
#             <venv>\bc250-rocm-attach.json with the four hashes, and an entry for the uninstaller in -ListDir.
#   -Detach   put AMD's DLLs back and remove ours, the record and the entry.
#   -Check    say the state and change nothing.
#   -PlanOnly say what attach or -Detach would do and change nothing.
# -ListDir is the list of attached venvs that the uninstaller reads, by default
# %ProgramData%\amdgpu-wddm\rocm-venvs: one file per venv, so that each user writes only their own entries. The
# uninstaller (tools\release\installer\rocm-venvs.ps1) runs -Detach for each entry, as an administrator.
# Exit codes: 0 attached (or detached with -Detach, or all attached or all detached with -Check); 2 a payload
# file or a venv directory is missing; 3 refused (a DLL that is neither ours nor the AMD DLL that payload.json
# names, a partly attached venv that -Check finds, a path that is not on a local drive letter, a junction or a
# link on the path); 4 a DLL to change is in use (close every Python process of the venv). A refusal changes no
# file.
# pip and this script: pip removes and installs by the names of the wheel's RECORD, which does not know the
# kept names. A reinstall of the same wheel leaves state 'reinstalled' (attach again); a wheel of another
# TheRock build leaves state 'other' (install the build of payload.json, then attach). A venv that an earlier
# payload of ours attached (the release before an upgrade) is state 'earlier', from the record in the venv:
# attach replaces our DLLs with the current ones, -Detach puts AMD's back.
#
# Windows PowerShell 5.1 and PowerShell 7: no ternary, null-coalescing or pipeline chain operators.
param(
    [Parameter(Mandatory = $true)][string]$Venv,
    [string]$Payload = '',
    [string]$ListDir = '',
    [switch]$Detach,
    [switch]$Check,
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $Payload) { $Payload = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $ListDir) { $ListDir = Join-Path $env:ProgramData 'amdgpu-wddm\rocm-venvs' }

function Sha([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Say([string]$Text) { Write-Host "ROCM-ATTACH $Text" }
function Stop-With([int]$Code, [string]$Text) { Say $Text; exit $Code }
function Get-Field($Object, [string]$Name) {
    if ($null -eq $Object -or -not $Object.PSObject.Properties[$Name]) { return '' }
    return [string]$Object.$Name
}

# The uninstaller runs -Detach as an administrator on a path from -ListDir, which every user can write. Only a
# folder on a local drive letter is changed: a network path would make the administrator's account log on to the
# server that the path names.
if (-not [IO.Path]::IsPathRooted($Venv)) { $Venv = Join-Path (Get-Location).ProviderPath $Venv }
$Venv = [IO.Path]::GetFullPath($Venv).TrimEnd('\')
if ($Venv -notmatch '^[A-Za-z]:\\[^\\]') { Stop-With 3 "REFUSED: $Venv is not a folder on a local drive letter; nothing is changed" }
$site = Join-Path $Venv 'Lib\site-packages'
$coreBin = Join-Path $site '_rocm_sdk_core\bin'
$libBin = Join-Path $site '_rocm_sdk_libraries\bin'
foreach ($d in @($coreBin, $libBin)) {
    if (-not (Test-Path -LiteralPath $d)) { Stop-With 2 "MISSING: $d (is $Venv a TheRock venv?)" }
}
$manifestPath = Join-Path $Payload 'payload.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { Stop-With 2 "MISSING: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

# The record of the last attach in this venv: which of our DLLs it put in, and which AMD DLLs it kept.
$record = Join-Path $Venv 'bc250-rocm-attach.json'
$rec = $null
if (Test-Path -LiteralPath $record) {
    try { $rec = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json } catch { $rec = $null }
}

# The two replacements: where ours goes, where AMD's is kept, and the hashes.
function New-Replacement([string]$What, [string]$Live, [string]$Kept, $Entry, $Earlier) {
    $ours = Join-Path $Payload ([string]$Entry.file)
    return [pscustomobject]@{
        what = $What; live = $Live; kept = $Kept; ours = $ours; oursSha = (Sha $ours); fixedSha = (Get-Field $Entry 'sha256')
        amdSha = [string]$Entry.amd_sha256; earlierSha = (Get-Field $Earlier 'sha256'); earlierAmd = (Get-Field $Earlier 'amd_sha256')
    }
}
$earlierFace = $null
$earlierFront = $null
if ($null -ne $rec) {
    if ($rec.PSObject.Properties['face']) { $earlierFace = $rec.face }
    if ($rec.PSObject.Properties['blaslt_front']) { $earlierFront = $rec.blaslt_front }
}
$items = @(
    (New-Replacement 'HIP runtime face' (Join-Path $coreBin 'amdhip64_7.dll') (Join-Path $coreBin 'amdhip64_7.dll.orig') $manifest.face $earlierFace),
    (New-Replacement 'hipBLASLt front' (Join-Path $libBin 'libhipblaslt.dll') (Join-Path $libBin 'libhipblaslt_amd.dll') $manifest.blaslt_front $earlierFront)
)
foreach ($i in $items) {
    if ($i.oursSha -and $i.fixedSha -and $i.oursSha -ne $i.fixedSha) {
        Stop-With 3 "REFUSED: $($i.ours) is not the file payload.json names ($($i.fixedSha)); nothing is changed"
    }
    if ($i.oursSha -and $i.oursSha -eq $i.amdSha) { Stop-With 3 "REFUSED: $($i.ours) has the bytes of AMD's DLL, so it is not ours; nothing is changed" }
    if (-not $Check -and -not $Detach -and -not $i.oursSha) { Stop-With 2 "MISSING: $($i.ours)" }
}

# The state of one replacement:
#   attached     ours live, AMD's kept;
#   earlier      the DLL of an earlier payload of ours live (the record in the venv names it), AMD's kept;
#   detached     AMD's live, nothing kept;
#   reinstalled  AMD's live and AMD's kept: pip installed the same wheel again over an attached venv (pip does not
#                know the kept name, so it leaves that file). Attach puts ours back; -Detach removes the kept copy.
#   other        anything else, for example another TheRock build: changed by no mode.
function Get-State($i) {
    $live = Sha $i.live
    $kept = Sha $i.kept
    if ($i.oursSha -and $live -eq $i.oursSha -and $kept -eq $i.amdSha) { return 'attached' }
    if ($i.earlierSha -and $live -eq $i.earlierSha -and $i.earlierAmd -and $kept -eq $i.earlierAmd) { return 'earlier' }
    if ($live -eq $i.amdSha -and $kept -eq '') { return 'detached' }
    if ($live -eq $i.amdSha -and $kept -eq $i.amdSha) { return 'reinstalled' }
    return "other (live '$live', kept '$kept')"
}

$states = @($items | ForEach-Object { Get-State $_ })
for ($n = 0; $n -lt $items.Count; $n++) { Say "$($items[$n].what): $($states[$n])" }

# The entry of this venv in the uninstaller's list: one file, named by the SHA-256 of the lower-case path.
$sha = [Security.Cryptography.SHA256]::Create()
$key = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Venv.ToLowerInvariant()))[0..15] | ForEach-Object { $_.ToString('x2') })
$entry = Join-Path $ListDir "$key.json"

$otherHint = "This payload is for TheRock $($manifest.therock): install exactly that rocm_sdk_core and rocm_sdk_libraries (pip install --force-reinstall), then attach again."
if ($Check) {
    if (Test-Path -LiteralPath $entry) { Say "recorded for the uninstaller: $entry" } else { Say "not recorded for the uninstaller ($entry)" }
    if (@($states | Where-Object { $_ -ne 'attached' }).Count -eq 0) { Stop-With 0 'CHECK attached' }
    if (@($states | Where-Object { $_ -ne 'detached' }).Count -eq 0) { Stop-With 0 'CHECK detached' }
    if (@($states | Where-Object { $_ -like 'other*' }).Count -eq 0) {
        if (@($states | Where-Object { $_ -eq 'earlier' }).Count) {
            Stop-With 3 'CHECK REFUSED: an earlier payload of ours is attached (the release before an upgrade). Attach again to put in the current one, or -Detach.'
        }
        Stop-With 3 'CHECK REFUSED: the venv is partly attached (a pip reinstall puts AMD''s DLL back over ours). Attach again, or -Detach.'
    }
    Stop-With 3 "CHECK REFUSED: an AMD DLL of this venv is not the one this payload names. $otherHint"
}

# A junction or a link on the path would let a user send the administrator's -Detach of the uninstaller into another
# folder. Every directory from the root of the drive to the two bin directories is opened without delete sharing and
# held until the end: while the handles are open, none of them can be renamed, removed or replaced by a junction,
# and a non-empty directory cannot be made one.
if (-not ('Bc250.RocmPathGuard' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace Bc250 {
public static class RocmPathGuard {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation {
        public uint Attributes; public long Created; public long Accessed; public long Written;
        public uint Volume; public uint SizeHigh; public uint SizeLow; public uint Links; public uint IndexHigh; public uint IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out FileInformation info);
    const uint ReadAttributes = 0x80, ShareReadWrite = 3, OpenExisting = 3, BackupSemantics = 0x02000000, OpenReparsePoint = 0x00200000;
    const uint AttributeDirectory = 0x10, AttributeReparsePoint = 0x400;
    static string Error() { return new Win32Exception(Marshal.GetLastWin32Error()).Message; }
    public static List<SafeFileHandle> Hold(string[] dirs) {
        List<SafeFileHandle> held = new List<SafeFileHandle>();
        HashSet<string> seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try {
            foreach (string dir in dirs) {
                string full = Path.GetFullPath(dir).TrimEnd('\\');
                string root = Path.GetPathRoot(full);
                List<string> chain = new List<string>();
                chain.Add(root);
                string current = root;
                foreach (string part in full.Substring(root.Length).Split(new char[] { '\\' }, StringSplitOptions.RemoveEmptyEntries)) {
                    current = Path.Combine(current, part);
                    chain.Add(current);
                }
                foreach (string path in chain) {
                    if (!seen.Add(path)) continue;
                    SafeFileHandle h = CreateFileW(path, ReadAttributes, ShareReadWrite, IntPtr.Zero, OpenExisting, BackupSemantics | OpenReparsePoint, IntPtr.Zero);
                    if (h.IsInvalid) throw new IOException(path + " cannot be opened: " + Error());
                    held.Add(h);
                    FileInformation info;
                    if (!GetFileInformationByHandle(h, out info)) throw new IOException(path + ": no file information: " + Error());
                    if ((info.Attributes & AttributeReparsePoint) != 0) throw new IOException(path + " is a junction or a link");
                    if ((info.Attributes & AttributeDirectory) == 0) throw new IOException(path + " is not a directory");
                }
            }
        } catch {
            foreach (SafeFileHandle h in held) h.Dispose();
            throw;
        }
        return held;
    }
}
}
'@
}
try { $script:held = [Bc250.RocmPathGuard]::Hold(@($coreBin, $libBin)) }
catch {
    $why = $_.Exception
    if ($null -ne $why.InnerException) { $why = $why.InnerException }
    Stop-With 3 "REFUSED: $($why.Message); nothing is changed"
}
foreach ($i in $items) {
    foreach ($f in @($i.live, $i.kept)) {
        if ((Test-Path -LiteralPath $f) -and (([IO.File]::GetAttributes($f) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            Stop-With 3 "REFUSED: $f is a link; nothing is changed"
        }
    }
}

# A DLL that a process has loaded can be renamed but not removed or overwritten: a change of the venv that
# stops halfway leaves it half attached. Every file that a mode changes is opened without sharing first.
function Assert-NotInUse([string[]]$Paths) {
    foreach ($p in $Paths) {
        if (-not (Test-Path -LiteralPath $p)) { continue }
        try { $s = [IO.File]::Open($p, 'Open', 'ReadWrite', 'None'); $s.Close() }
        catch { Stop-With 4 "IN USE: $p (close every Python process of $Venv, then run again)" }
    }
}
$changing = @()
for ($n = 0; $n -lt $items.Count; $n++) {
    $want = 'attached'
    if ($Detach) { $want = 'detached' }
    if ($states[$n] -ne $want) { $changing += @($items[$n].live, $items[$n].kept) }
}

if ($Detach) {
    for ($n = 0; $n -lt $items.Count; $n++) {
        $i = $items[$n]
        if ($states[$n] -like 'other*') {
            Stop-With 3 "REFUSED: $($i.what) is $($states[$n]); nothing is changed. If pip installed another TheRock build, AMD's DLLs are live already: only the kept copies $($items[0].kept) and $($items[1].kept) are left, and they can be deleted by hand."
        }
    }
    if ($PlanOnly) { Stop-With 0 'PLAN detach: the AMD DLLs go back in place, ours, the record and the entry for the uninstaller are removed' }
    Assert-NotInUse $changing
    for ($n = 0; $n -lt $items.Count; $n++) {
        $i = $items[$n]
        $amd = $i.amdSha
        if ($states[$n] -eq 'earlier') { $amd = $i.earlierAmd }
        if ($states[$n] -eq 'detached') { Say "$($i.what): already detached"; continue }
        if ($states[$n] -eq 'reinstalled') {
            Remove-Item -LiteralPath $i.kept -Force
        } else {
            Remove-Item -LiteralPath $i.live -Force
            Move-Item -LiteralPath $i.kept -Destination $i.live
        }
        if ((Sha $i.live) -ne $amd -or (Test-Path -LiteralPath $i.kept)) {
            Stop-With 3 "REFUSED: after the detach $($i.live) is not AMD's DLL alone"
        }
        Say "$($i.what): detached, $($i.live) is AMD's $amd"
    }
    if (Test-Path -LiteralPath $record) { Remove-Item -LiteralPath $record -Force }
    if (Test-Path -LiteralPath $entry) {
        try { Remove-Item -LiteralPath $entry -Force } catch { Say "WARNING: the entry for the uninstaller stays ($($_.Exception.Message)): $entry" }
    }
    Stop-With 0 'DETACHED'
}

for ($n = 0; $n -lt $items.Count; $n++) {
    $i = $items[$n]
    if ($states[$n] -like 'other*') {
        Stop-With 3 "REFUSED: $($i.what) is $($states[$n]); nothing is changed. $($i.live) must be AMD's $($i.amdSha) or ours. $otherHint"
    }
    if ($states[$n] -eq 'earlier' -and $i.earlierAmd -ne $i.amdSha) {
        Stop-With 3 "REFUSED: $($i.what) is an earlier payload of ours over AMD's $($i.earlierAmd), another TheRock build than this payload's; nothing is changed. Run -Detach, then: $otherHint"
    }
}
if ($PlanOnly) { Stop-With 0 'PLAN attach: each detached AMD DLL is kept under its other name and ours takes its place' }
Assert-NotInUse $changing
for ($n = 0; $n -lt $items.Count; $n++) {
    $i = $items[$n]
    if ($states[$n] -eq 'attached') { Say "$($i.what): already attached"; continue }
    if ($states[$n] -eq 'reinstalled' -or $states[$n] -eq 'earlier') { Remove-Item -LiteralPath $i.live -Force }
    else { Move-Item -LiteralPath $i.live -Destination $i.kept }
    Copy-Item -LiteralPath $i.ours -Destination $i.live
    if ((Sha $i.live) -ne $i.oursSha -or (Sha $i.kept) -ne $i.amdSha) {
        Stop-With 3 "REFUSED: after the attach $($i.what) does not have the expected hashes"
    }
    Say "$($i.what): attached, $($i.live) is ours $($i.oursSha), AMD's kept as $($i.kept)"
}
$utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$rec = [ordered]@{
    utc = $utc; therock = $manifest.therock; payload = $Payload
    face = [ordered]@{ live = $items[0].live; sha256 = $items[0].oursSha; kept = $items[0].kept; amd_sha256 = $items[0].amdSha }
    blaslt_front = [ordered]@{ live = $items[1].live; sha256 = $items[1].oursSha; kept = $items[1].kept; amd_sha256 = $items[1].amdSha }
}
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText($record, ($rec | ConvertTo-Json -Depth 4), $utf8)
try {
    New-Item -ItemType Directory -Force $ListDir | Out-Null
    $line = [ordered]@{ venv = $Venv; utc = $utc; therock = $manifest.therock; payload = $Payload }
    [IO.File]::WriteAllText($entry, ($line | ConvertTo-Json), $utf8)
    Say "recorded for the uninstaller: $entry"
} catch {
    Say "WARNING: not recorded for the uninstaller ($($_.Exception.Message)): run -Detach before you uninstall the driver"
}
Stop-With 0 "ATTACHED (record $record)"
