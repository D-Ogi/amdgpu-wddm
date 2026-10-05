# The running-release witness (GUI plan F-VER and G-VER; format: docs/gui/interfaces.md section 1): a record that the
# driver started in THIS boot is the KMD of one named release package. Written by install.ps1's verify step and by the
# start-confirm task, which runs from <install root>\tools; read by the control application. Windows PowerShell 5.1
# syntax only.
#
# A driver reply carries the KMD identity (BC250_KMD_VERSION), not a release: repackaging can change the release and
# the INF version and leave the reply as it is. So the witness binds three things that are read now, never inferred:
#   * the KMD image that Windows loaded (the bc250kmd.sys module in the list of loaded drivers) has the SHA256 that the
#     release's manifest records for payload/kmd/bc250kmd.sys (that hash also fixes the manifest's kmd_build);
#   * the KMD's reply version (bc250kmd_cli info) equals the manifest's kmd_abi;
#   * the boot (BootId, the value the control application's BD-060 observer reads) and the time of the reading.
# Nothing else makes a witness: not Release\Version, not the newest release with a matching reply. When any part
# cannot be read or does not match, nothing is written and the reason goes to the caller's log. The reader rejects a
# witness of another boot and one that is older than an install action of the same boot (state.json mutation_*).
# The file lives in the installer's state folder, which only administrators and SYSTEM can write (Set-StateDirAccess);
# the reader also requires such an owner.

$script:WitnessSchema = 1
$script:WitnessPath = Join-Path $env:ProgramData 'amdgpu-wddm\installer\running-release.json'
$script:BootIdentity = $null

# The boot, defined here and nowhere else (the engine, verify, the start-confirm task and the witness all use it):
# boot_id is HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters\BootId, the
# REG_DWORD that Windows increments at every boot, read as an unsigned 32-bit number (the control application reads the
# same value the same way); boot_utc is Win32_OperatingSystem.LastBootUpTime in UTC. Read once per process; a part
# that cannot be read is $null.
function Get-BootIdentity {
    if ($script:BootIdentity) { return $script:BootIdentity }
    $id = $null
    try {
        $v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters' -Name BootId -ErrorAction Stop).BootId
        if ($null -ne $v) { $id = [int64][BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$v), 0) }
    } catch { }
    # Host tests only, in a dry run: AMDGPU_WDDM_TEST_BOOT_ID=unreadable stands for a BootId that cannot be read.
    if ($script:DryRunMode -and $env:AMDGPU_WDDM_TEST_BOOT_ID -eq 'unreadable') { $id = $null }
    $utc = $null
    try { $utc = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o') } catch { }
    $script:BootIdentity = [ordered]@{ boot_id = $id; boot_utc = $utc }
    return $script:BootIdentity
}

if (-not ('AmdgpuWddmWitness.Drivers' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace AmdgpuWddmWitness {
public static class Drivers {
    [DllImport("psapi.dll", SetLastError = true)]
    static extern bool EnumDeviceDrivers([Out] IntPtr[] bases, int cb, out int needed);
    [DllImport("psapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern int GetDeviceDriverFileNameW(IntPtr imageBase, StringBuilder name, int size);
    // The file names of the loaded kernel modules as Windows reports them (\SystemRoot\..., \??\C:\...).
    public static string[] Loaded() {
        int needed;
        EnumDeviceDrivers(null, 0, out needed);
        if (needed <= 0) return new string[0];
        var bases = new IntPtr[needed / IntPtr.Size + 16];
        if (!EnumDeviceDrivers(bases, bases.Length * IntPtr.Size, out needed)) return new string[0];
        var r = new List<string>();
        for (int i = 0; i < Math.Min(bases.Length, needed / IntPtr.Size); i++) {
            if (bases[i] == IntPtr.Zero) continue;
            var sb = new StringBuilder(1024);
            if (GetDeviceDriverFileNameW(bases[i], sb, sb.Capacity) > 0) r.Add(sb.ToString());
        }
        return r.ToArray();
    }
}
}
'@
}

# A kernel module name as a Win32 path.
function ConvertTo-Win32DriverPath([string]$Name) {
    if (-not $Name) { return $null }
    $p = $Name
    if ($p -match '^\\SystemRoot\\(.*)$') { $p = Join-Path $env:windir $Matches[1] }
    elseif ($p -match '^\\\?\?\\(.*)$') { $p = $Matches[1] }
    elseif ($p -match '^\\Windows\\(.*)$') { $p = Join-Path $env:windir $Matches[1] }
    return $p
}

# What the started KMD is: the loaded image (path, SHA256) and the reply version. A missing part is $null.
function Get-RunningKmdReading([string]$Cli, [string]$ImageName = 'bc250kmd.sys') {
    $image = $null
    try {
        $mod = @([AmdgpuWddmWitness.Drivers]::Loaded() | Where-Object { ([IO.Path]::GetFileName($_)) -ieq $ImageName }) | Select-Object -First 1
        if ($mod) {
            $path = ConvertTo-Win32DriverPath $mod
            $sha = $null; $created = $null; $written = $null
            if ($path -and (Test-Path -LiteralPath $path -PathType Leaf)) {
                $s = [IO.File]::OpenRead($path)
                try { $sha = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($s)) -replace '-', '') } finally { $s.Dispose() }
                $fi = New-Object IO.FileInfo $path
                $created = $fi.CreationTimeUtc.ToString('o'); $written = $fi.LastWriteTimeUtc.ToString('o')
            }
            $image = [ordered]@{ module = $mod; path = $path; sha256 = $sha; created_utc = $created; written_utc = $written }
        }
    } catch { }
    $reply = $null
    if ($Cli -and (Test-Path -LiteralPath $Cli)) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { $text = (& $Cli 'info' 2>&1 | ForEach-Object { [string]$_ }) -join "`n"; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
        if ($code -eq 0 -and $text -match '(?m)^version\s+(0x[0-9A-Fa-f]{8})') { $reply = $Matches[1].ToUpperInvariant() -replace '^0X', '0x' }
    }
    return [ordered]@{ image = $image; reply_version = $reply }
}

# Pure: the witness record for one reading, or the reason why there is none. $Manifest is the installed release's
# manifest.json, $ManifestSha256 the SHA256 of that file. The fields of section 1 first; boot_utc, kmd_image_path,
# reply_version and driver_ver are for the support report only.
# Every field of section 1 is always written, never empty: a reading or a manifest that cannot fill one gives no
# witness. The loaded image is the file at the path of the bc250kmd.sys module that Windows lists as loaded; that file
# is the image of this boot only while nothing replaced it after the boot started, so there is no witness when the
# file was created or written after boot_utc, or when $State records an install action in this boot.
function Get-RunningReleaseWitness {
    param($Manifest, [string]$ManifestSha256, $Reading, $Boot, [ValidateSet('verify', 'start-confirm')][string]$RecordedBy, [string]$Utc = ([DateTime]::UtcNow.ToString('o')), $State)
    function No([string]$Why) { return [pscustomobject]@{ record = $null; reason = $Why } }
    function Get-Utc([string]$T) {
        $d = [DateTime]::MinValue
        if ($T -and [DateTime]::TryParse($T, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]'AdjustToUniversal, AssumeUniversal', [ref]$d)) { return $d }
        return $null
    }
    if (-not $Manifest) { return No 'no release manifest in the install root' }
    if ($ManifestSha256 -notmatch '^[0-9A-Fa-f]{64}$') { return No 'the installed manifest.json has no SHA256' }
    if (-not [string]$Manifest.name -or -not [string]$Manifest.version) { return No 'the manifest names no release or version' }
    if ([string]$Manifest.kmd_build -notmatch '^\d+\.\d+\.\d+\.\d+$') { return No "the manifest's kmd_build '$($Manifest.kmd_build)' is not a four-part build" }
    if ([string]$Manifest.kmd_abi -notmatch '^0x[0-9A-Fa-f]{8}$') { return No "the manifest's kmd_abi '$($Manifest.kmd_abi)' is not 0x followed by 8 hex digits" }
    if (-not $Boot -or $null -eq $Boot.boot_id) { return No 'the boot cannot be identified (BootId unreadable)' }
    $bootUtc = Get-Utc ([string]$Boot.boot_utc)
    if ($null -eq $bootUtc) { return No 'the start time of this boot cannot be read' }
    if ($State -and $State.PSObject.Properties['mutation_boot_id'] -and $null -ne $State.mutation_boot_id -and [int64]$State.mutation_boot_id -eq [int64]$Boot.boot_id) {
        return No "an install action ran in this boot ($($State.mutation_utc)): the loaded image may differ from the files; the next start writes the witness"
    }
    $comp = @($Manifest.components | Where-Object { $_ -and $_.package_path -eq 'payload/kmd/bc250kmd.sys' }) | Select-Object -First 1
    if (-not $comp -or -not $comp.sha256) { return No 'the manifest names no KMD image hash' }
    if (-not $Reading -or -not $Reading.image) { return No 'no bc250kmd.sys among the loaded drivers' }
    if (-not $Reading.image.sha256) { return No "the loaded image $($Reading.image.module) cannot be read" }
    $created = Get-Utc ([string]$Reading.image.created_utc); $written = Get-Utc ([string]$Reading.image.written_utc)
    if ($null -eq $created -or $null -eq $written) { return No "the times of the loaded image $($Reading.image.path) cannot be read" }
    if ($created -gt $bootUtc -or $written -gt $bootUtc) { return No "the loaded image $($Reading.image.path) was replaced after this boot started (created $($Reading.image.created_utc), written $($Reading.image.written_utc), boot $($Boot.boot_utc))" }
    if ($Reading.image.sha256 -ne ([string]$comp.sha256).ToUpperInvariant()) { return No "the loaded image $($Reading.image.path) has SHA256 $($Reading.image.sha256), release $($Manifest.version) has $($comp.sha256)" }
    if (-not $Reading.reply_version) { return No 'no driver reply (bc250kmd_cli info)' }
    if ($Reading.reply_version -ne ([string]$Manifest.kmd_abi)) { return No "the driver replies $($Reading.reply_version), release $($Manifest.version) has kmd_abi $($Manifest.kmd_abi)" }
    $rec = [ordered]@{
        schema = $script:WitnessSchema
        boot_id = [int64]$Boot.boot_id
        recorded_utc = $Utc
        recorded_by = $RecordedBy
        release = [string]$Manifest.name
        version = [string]$Manifest.version
        manifest_sha256 = $ManifestSha256.ToUpperInvariant()
        kmd_image_sha256 = $Reading.image.sha256
        kmd_build = [string]$Manifest.kmd_build
        kmd_abi = [string]$Manifest.kmd_abi
        boot_utc = $Boot.boot_utc
        kmd_image_path = $Reading.image.path
        reply_version = $Reading.reply_version
        driver_ver = [string]$Manifest.kmd_version
    }
    return [pscustomobject]@{ record = $rec; reason = $null }
}

# Pure: does a witness name what runs in this boot? The reader's rule of section 1 (the control application's
# DriverCard.WitnessProblem implements the same); $Reply is the driver's reply version, $State the installer state.
function Test-RunningReleaseWitness {
    param($Witness, $Boot, [string]$Reply, $State)
    function Get-Utc([string]$T) {
        $d = [DateTime]::MinValue
        if ($T -and [DateTime]::TryParse($T, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]'AdjustToUniversal, AssumeUniversal', [ref]$d)) { return $d }
        return $null
    }
    if (-not $Witness -or [string]$Witness.schema -ne [string]$script:WitnessSchema -or $Witness.recorded_by -notin 'verify', 'start-confirm') { return [pscustomobject]@{ valid = $false; reason = 'no witness' } }
    if (-not $Boot -or $null -eq $Boot.boot_id -or [int64]$Witness.boot_id -ne [int64]$Boot.boot_id) { return [pscustomobject]@{ valid = $false; reason = 'the witness is of another boot' } }
    if (-not $Reply -or [string]$Witness.kmd_abi -ne $Reply) { return [pscustomobject]@{ valid = $false; reason = "the witness names kmd_abi $($Witness.kmd_abi), the driver replies $Reply" } }
    $w = Get-Utc ([string]$Witness.recorded_utc)
    if ($null -eq $w) { return [pscustomobject]@{ valid = $false; reason = 'the witness has no valid time' } }
    if ($State -and $State.PSObject.Properties['mutation_boot_id'] -and $null -ne $State.mutation_boot_id -and [int64]$State.mutation_boot_id -eq [int64]$Boot.boot_id) {
        $t = Get-Utc ([string]$State.mutation_utc)
        if ($null -eq $t -or $t -gt $w) { return [pscustomobject]@{ valid = $false; reason = "an install action at $($State.mutation_utc) came after the witness in this boot" } }
    }
    return [pscustomobject]@{ valid = $true; reason = $null }
}

# Reads the running KMD and writes the witness for the release installed in $InstallRoot. Returns the reason when
# nothing was written. Never throws.
# manifest_sha256 is the SHA256 of exactly one file, <install root>\manifest.json, the manifest of the installed
# release; $StatePath is the installer's state.json (its mutation_* fields, docs/gui/interfaces.md section 2). The
# written file gets Administrators as its owner when the writer is not SYSTEM (the reader trusts only those two); a
# file whose owner cannot be set is removed again.
# The whole sequence (state, manifest and driver readings, then the publication) runs under the installer's
# engine.lock, the lock every changing installer run holds for its life (engine.ps1 Enter-EngineLock): no install
# action can start between the readings and the publication. Verify holds the lock already (-LockHeld); the
# start-confirm task takes it, waiting up to -LockWaitMs, and writes no witness while an installer runs. Right before
# the publication the state is read once more: a record of an install action of this boot that appeared since (a
# writer that ignored the lock) still gives no witness. -Reading and -BeforePublish are for host tests only.
function Write-RunningReleaseWitness {
    param([Parameter(Mandatory)][string]$InstallRoot, [Parameter(Mandatory)][ValidateSet('verify', 'start-confirm')][string]$RecordedBy, $Boot, [string]$Path = $script:WitnessPath,
        [string]$StatePath = (Join-Path $env:ProgramData 'amdgpu-wddm\installer\state.json'), [switch]$LockHeld, [int]$LockWaitMs = 3000,
        $Reading, [scriptblock]$BeforePublish)
    $lock = $null
    try {
        if (-not $LockHeld) {
            $lock = Open-InstallerLock -Directory (Split-Path $StatePath) -WaitMs $LockWaitMs
            if (-not $lock) { return 'an installer run holds engine.lock: no witness until the next start' }
        }
        $mp = Join-Path $InstallRoot 'manifest.json'
        if (-not (Test-Path -LiteralPath $mp)) { return "no manifest.json in $InstallRoot" }
        $bytes = [IO.File]::ReadAllBytes($mp)
        $msha = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)) -replace '-', '')
        $manifest = (New-Object Text.UTF8Encoding $false).GetString($bytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
        if (-not $Boot) { $Boot = Get-BootIdentity }
        $st = $null
        if ($StatePath -and (Test-Path -LiteralPath $StatePath)) { $st = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json }
        if (-not $Reading) { $Reading = Get-RunningKmdReading -Cli (Join-Path $InstallRoot 'tools\bc250kmd_cli.exe') }
        $w = Get-RunningReleaseWitness -Manifest $manifest -ManifestSha256 $msha -Reading $Reading -Boot $Boot -RecordedBy $RecordedBy -State $st
        if (-not $w.record) { return $w.reason }
        if ($BeforePublish) { & $BeforePublish }
        if ($StatePath -and (Test-Path -LiteralPath $StatePath)) {
            $again = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
            if ($again -and $again.PSObject.Properties['mutation_boot_id'] -and $null -ne $again.mutation_boot_id -and [int64]$again.mutation_boot_id -eq [int64]$Boot.boot_id) {
                return "an install action of this boot was recorded during the reading ($($again.mutation_utc)): no witness"
            }
        }
        [void][IO.Directory]::CreateDirectory((Split-Path $Path))
        $tmp = $Path + '.tmp-' + [guid]::NewGuid().ToString('N')
        [IO.File]::WriteAllText($tmp, ($w.record | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding $false))
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) } else { [IO.File]::Move($tmp, $Path) }
        $why = Set-AdminOwner $Path
        if ($why) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue; return "witness removed again: $why" }
        return $null
    } catch { return "witness not written: $($_.Exception.Message)" }
    finally { if ($lock) { $lock.Dispose() } }
}

# The installer's engine.lock (engine.ps1 Enter-EngineLock opens it the same way), waited for up to $WaitMs; $null
# when another process keeps it. The caller disposes the handle.
function Open-InstallerLock([string]$Directory, [int]$WaitMs = 10000) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        try {
            [void][IO.Directory]::CreateDirectory($Directory)
            return (New-Object IO.FileStream -ArgumentList (Join-Path $Directory 'engine.lock'), ([IO.FileMode]::OpenOrCreate), ([IO.FileAccess]::ReadWrite), ([IO.FileShare]::None))
        } catch [IO.IOException] {
            if ($clock.ElapsedMilliseconds -ge $WaitMs) { return $null }
            Start-Sleep -Milliseconds 250
        }
    }
}

# The owner the control application trusts: Administrators, or SYSTEM when SYSTEM wrote the file. $null when set.
function Set-AdminOwner([string]$Path) {
    try {
        $admins = New-Object Security.Principal.SecurityIdentifier ([Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)
        $system = New-Object Security.Principal.SecurityIdentifier ([Security.Principal.WellKnownSidType]::LocalSystemSid, $null)
        $acl = Get-Acl -LiteralPath $Path
        $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier])
        if ($owner -eq $admins -or $owner -eq $system) { return $null }
        $acl.SetOwner($admins)
        Set-Acl -LiteralPath $Path -AclObject $acl
        $owner = (Get-Acl -LiteralPath $Path).GetOwner([Security.Principal.SecurityIdentifier])
        if ($owner -eq $admins) { return $null }
        return 'the owner stays another account, not Administrators'
    } catch { return "the owner cannot be set: $($_.Exception.Message)" }
}
