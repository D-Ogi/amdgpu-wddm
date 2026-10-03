# The DWM instance of the desktop session (BD-060), shared by install.ps1 and the start-confirm task, which runs from
# <install root>\tools. Windows PowerShell 5.1 syntax only.
#
# WinUI pointer-input loss after the desktop compositor (DWM) is terminated and restarted reproduces on this Windows
# build also with Microsoft Basic Display; restarting Windows recovers. Only a replacement that was seen is reported:
# the start-confirm task records the session's DWM (process ID and creation time) at each logon in
# %ProgramData%\amdgpu-wddm\dwm-baseline.json, keyed by the boot, the session and its logon time, and a later reading
# of the same session that finds another instance has observed a replacement. Without a record for the session the
# history is unknown: never "restarted", never "healthy". The finding helps to attribute a failure. It never makes a
# bug report unnecessary: a DWM crash can still be a driver defect.

$script:DwmBaselinePath = Join-Path $env:ProgramData 'amdgpu-wddm\dwm-baseline.json'
$script:DwmBaselineKeep = 16

if (-not ('AmdgpuWddmInstaller.Wts' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AmdgpuWddmInstaller {
public static class Wts {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WTSINFOW {
        public int State, SessionId, IncomingBytes, OutgoingBytes, IncomingFrames, OutgoingFrames, IncomingCompressedBytes, OutgoingCompressedBytes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string WinStationName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 17)] public string Domain;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 21)] public string UserName;
        public long ConnectTime, DisconnectTime, LastInputTime, LogonTime, CurrentTime;
    }
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool WTSQuerySessionInformationW(IntPtr server, int sessionId, int infoClass, out IntPtr buffer, out int bytes);
    [DllImport("wtsapi32.dll")]
    static extern void WTSFreeMemory(IntPtr memory);
    [DllImport("kernel32.dll")]
    static extern uint WTSGetActiveConsoleSessionId();
    // WTSSessionInfo (24): the session's logon time as a FILETIME, 0 when nobody is logged on; -1 on failure.
    public static long LogonTime(int sessionId) {
        IntPtr buffer; int bytes;
        if (!WTSQuerySessionInformationW(IntPtr.Zero, sessionId, 24, out buffer, out bytes)) return -1;
        try { return ((WTSINFOW)Marshal.PtrToStructure(buffer, typeof(WTSINFOW))).LogonTime; } finally { WTSFreeMemory(buffer); }
    }
    public static long ConsoleSession() { return WTSGetActiveConsoleSessionId(); }
}
}
'@
}

function Format-DwmTime($T) { return ([datetime]$T).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss') + 'Z' }
function ConvertFrom-DwmTime([string]$Text) {
    return [DateTime]::Parse($Text, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
}
# The session with the desktop: this process's own, or the console session when this runs in session 0 (a service,
# an SSH shell). 0: none.
function Get-DesktopSessionId {
    $own = (Get-Process -Id $PID).SessionId
    if ($own -ne 0) { return [int]$own }
    $c = [AmdgpuWddmInstaller.Wts]::ConsoleSession()
    if ($c -lt 0 -or $c -eq 4294967295) { return 0 }
    return [int]$c
}
# The logon time of a session (WTSQuerySessionInformation, WTSSessionInfo), UTC; $null when nobody is logged on.
function Get-SessionLogonUtc([int]$SessionId) {
    $t = [AmdgpuWddmInstaller.Wts]::LogonTime($SessionId)
    if ($t -le 0) { return $null }
    return [DateTime]::FromFileTimeUtc($t)
}
# The session's dwm.exe processes: ID and creation time, UTC (Win32_Process, readable without elevation).
function Get-SessionDwm([int]$SessionId) {
    return , @(Get-CimInstance Win32_Process -Filter "Name='dwm.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $SessionId -and $_.CreationDate } | ForEach-Object { [pscustomobject]@{ pid = [int]$_.ProcessId; created_utc = $_.CreationDate.ToUniversalTime() } })
}
# The epoch a baseline belongs to: this boot, the desktop session and its logon. A new sign-in is a new epoch.
function Get-DwmEpoch {
    $session = Get-DesktopSessionId
    $logon = $null
    if ($session -ne 0) { $logon = Get-SessionLogonUtc $session }
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
    return [pscustomobject]@{ boot_utc = $boot; session = $session; logon_utc = $logon }
}
function Format-DwmInstance($Instance) { return "DWM $($Instance.pid) (created $(Format-DwmTime $Instance.created_utc))" }
# Two readings name the same process: the same ID and the same creation time (1 s apart at most: other readers take it
# from another source at another precision).
function Test-SameDwm($A, $B) {
    return ([int]$A.pid -eq [int]$B.pid) -and ([math]::Abs((([datetime]$A.created_utc) - ([datetime]$B.created_utc)).TotalSeconds) -le 1)
}

function Read-DwmBaseline([string]$Path = $script:DwmBaselinePath) {
    if (-not (Test-Path -LiteralPath $Path)) { return , @() }
    try { return , @((Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json).records) } catch { return , @() }
}
# The record of an epoch: the same session, the boot and the logon within 2 s.
function Find-DwmBaseline($Records, $Epoch) {
    if (($Epoch.session -eq 0) -or ($null -eq $Epoch.logon_utc)) { return $null }
    foreach ($r in @($Records)) {
        if (-not $r) { continue }
        try {
            if (([int]$r.session -eq $Epoch.session) -and
                ([math]::Abs(((ConvertFrom-DwmTime $r.boot_utc) - $Epoch.boot_utc).TotalSeconds) -le 2) -and
                ([math]::Abs(((ConvertFrom-DwmTime $r.logon_utc) - $Epoch.logon_utc).TotalSeconds) -le 2)) { return $r }
        } catch { }
    }
    return $null
}
# The start-confirm task at each logon: the first DWM seen in an epoch is its baseline; a later call of the same epoch
# keeps that record. Returns the epoch's record and whether this call wrote it.
function Save-DwmBaseline($Epoch, $Current, [string]$Path = $script:DwmBaselinePath, [string]$By = 'start-confirm') {
    $records = Read-DwmBaseline $Path
    $have = Find-DwmBaseline $records $Epoch
    if ($have) { return [pscustomobject]@{ record = $have; written = $false } }
    if (($Epoch.session -eq 0) -or ($null -eq $Epoch.logon_utc) -or (@($Current).Count -ne 1)) { return [pscustomobject]@{ record = $null; written = $false } }
    $d = @($Current)[0]
    $rec = [pscustomobject][ordered]@{
        boot_utc = $Epoch.boot_utc.ToString('o'); session = $Epoch.session; logon_utc = $Epoch.logon_utc.ToString('o')
        dwm_pid = $d.pid; dwm_created_utc = ([datetime]$d.created_utc).ToUniversalTime().ToString('o')
        recorded_utc = [DateTime]::UtcNow.ToString('o'); recorded_by = $By
    }
    $all = @(@($records | Where-Object { $_ }) + @($rec))
    if ($all.Count -gt $script:DwmBaselineKeep) { $all = @($all | Select-Object -Last $script:DwmBaselineKeep) }
    $json = [ordered]@{ schema = 1; records = $all } | ConvertTo-Json -Depth 4
    $dir = Split-Path -Parent $Path
    [void][IO.Directory]::CreateDirectory($dir)
    $tmp = "$Path.tmp"
    [IO.File]::WriteAllText($tmp, $json, (New-Object Text.UTF8Encoding $false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
    return [pscustomobject]@{ record = $rec; written = $true }
}
# observed: the recorded instance is gone and another runs (a replacement seen); same: the recorded instance runs,
# which says nothing about a replacement before the record; unknown: no record for this session, no desktop session,
# or no DWM.
function Get-DwmReplacementFinding($Record, $Current, $Epoch) {
    if (($Epoch.session -eq 0) -or ($null -eq $Epoch.logon_utc)) { return [pscustomobject]@{ state = 'unknown'; detail = 'unknown: no desktop session with a logon' } }
    $now = @($Current)
    if (-not $now.Count) { return [pscustomobject]@{ state = 'unknown'; detail = "unknown: no DWM in session $($Epoch.session)" } }
    if (-not $Record) { return [pscustomobject]@{ state = 'unknown'; detail = "unknown history: no DWM of session $($Epoch.session) was recorded since the logon at $(Format-DwmTime $Epoch.logon_utc) (the start-confirm task records it after each logon of an administrator)" } }
    $base = [pscustomobject]@{ pid = [int]$Record.dwm_pid; created_utc = (ConvertFrom-DwmTime $Record.dwm_created_utc) }
    $recorded = Format-DwmTime (ConvertFrom-DwmTime $Record.recorded_utc)
    $running = @($now | Where-Object { Test-SameDwm $_ $base })
    if ($running.Count) { return [pscustomobject]@{ state = 'same'; detail = "same DWM instance since the record at ${recorded}: $(Format-DwmInstance $base) (an earlier replacement is not excluded)" } }
    $others = ($now | ForEach-Object { Format-DwmInstance $_ }) -join ', '
    return [pscustomobject]@{ state = 'observed'; detail = "observed: $(Format-DwmInstance $base), recorded at $recorded, was replaced by $others. WinUI pointer-input loss after the desktop compositor (DWM) is terminated and restarted reproduces on this Windows build also with Microsoft Basic Display; restart Windows to recover. A DWM crash can still be a driver defect: report it with a bug report." }
}
# Two readings of the session's DWM around a step (the driver package of an upgrade): a process observation, kept apart
# from the device's own outcome.
function Compare-DwmReadings($Before, $After) {
    $b = @($Before); $a = @($After)
    if (-not $b.Count -or -not $a.Count) { return "DWM before: $(if ($b.Count) { ($b | ForEach-Object { Format-DwmInstance $_ }) -join ', ' } else { 'none' }); after: $(if ($a.Count) { ($a | ForEach-Object { Format-DwmInstance $_ }) -join ', ' } else { 'none' })" }
    $kept = @($b | Where-Object { $x = $_; @($a | Where-Object { Test-SameDwm $_ $x }).Count })
    if ($kept.Count -eq $b.Count -and $a.Count -eq $b.Count) { return "same DWM instance: $(($a | ForEach-Object { Format-DwmInstance $_ }) -join ', ')" }
    return "DWM replaced: $(($b | ForEach-Object { Format-DwmInstance $_ }) -join ', ') -> $(($a | ForEach-Object { Format-DwmInstance $_ }) -join ', ')"
}
