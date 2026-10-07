# Display modes, stage A lab helper (docs/design/display-modes.md, "The lab plan for stage A").
#
# Runs in the console session (modes-trial.ps1 starts it there as a scheduled task; SSH runs in session 0, where
# ChangeDisplaySettingsEx has no desktop). Never writes the registry: every mode is set without CDS_UPDATEREGISTRY,
# so a restart always comes back to the registry mode, and the tool itself goes back to it when the hold ends.
#
#   modes.ps1 -List                                   the modes Windows offers, the current and the registry mode
#   modes.ps1 -Width 1920 -Height 1080 [-Fixed default|stretch|center] [-HoldSeconds 20]
#   modes.ps1 -Revert                                 back to the registry mode now
#
# -Fixed is DEVMODE.dmDisplayFixedOutput: default lets the driver choose (aspect ratio on this driver), stretch and
# center ask for the VidPN scaling of the same name. -Out FILE appends every line to FILE too.
param(
    [switch]$List,
    [switch]$Revert,
    [int]$Width = 0,
    [int]$Height = 0,
    [ValidateSet('default', 'stretch', 'center')][string]$Fixed = 'default',
    [ValidateRange(1, 60)][int]$HoldSeconds = 20,
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Bc250Modes {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields;
        public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettingsEx(string device, int mode, ref DEVMODE dm, int flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int ChangeDisplaySettingsEx(string device, ref DEVMODE dm, IntPtr hwnd, int flags, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "ChangeDisplaySettingsEx")]
    public static extern int ChangeDisplaySettingsExNull(string device, IntPtr dm, IntPtr hwnd, int flags, IntPtr param);
    // The device name is always NULL (the primary display). PowerShell passes a $null string as "", which names no
    // device, so the NULL is given here and never from the script.
    public static bool Get(int mode, out DEVMODE dm) {
        dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        return EnumDisplaySettingsEx(null, mode, ref dm, 0);
    }
    public static int Set(DEVMODE dm) { return ChangeDisplaySettingsEx(null, ref dm, IntPtr.Zero, 0, IntPtr.Zero); }
    public static int Registry() { return ChangeDisplaySettingsExNull(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero); }
}
'@

$ENUM_CURRENT = -1; $ENUM_REGISTRY = -2
$DM_PELSWIDTH = 0x80000; $DM_PELSHEIGHT = 0x100000; $DM_DISPLAYFIXEDOUTPUT = 0x20000000
$fixedValue = @{ default = 0; stretch = 1; center = 2 }[$Fixed]

function Say([string]$line) {
    $stamp = (Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff')
    Write-Output "$stamp $line"
    if ($Out) { Add-Content -LiteralPath $Out -Value "$stamp $line" }
}
function Mode([int]$index) {
    $d = New-Object Bc250Modes+DEVMODE
    if (-not [Bc250Modes]::Get($index, [ref]$d)) { return $null }
    return $d
}
function Text($d) {
    if ($null -eq $d) { return 'none' }
    return "$($d.dmPelsWidth)x$($d.dmPelsHeight) $($d.dmBitsPerPel) bpp $($d.dmDisplayFrequency) Hz fixed-output $($d.dmDisplayFixedOutput)"
}
function BackToRegistry {
    $r = [Bc250Modes]::Registry()
    Say "revert: ChangeDisplaySettingsEx(registry mode) = $r; current $(Text (Mode $ENUM_CURRENT))"
}

Say "current $(Text (Mode $ENUM_CURRENT)); registry $(Text (Mode $ENUM_REGISTRY))"
if ($List) {
    $seen = @{}
    for ($i = 0; $i -lt 4096; $i++) {
        $d = Mode $i
        if ($null -eq $d) { break }
        $key = "$($d.dmPelsWidth)x$($d.dmPelsHeight) $($d.dmBitsPerPel) bpp $($d.dmDisplayFrequency) Hz"
        if (-not $seen.ContainsKey($key)) { $seen[$key] = $true; Say "mode $key" }
    }
    Say "modes: $($seen.Count) distinct"
    exit 0
}
if ($Revert) { BackToRegistry; exit 0 }
if ($Width -le 0 -or $Height -le 0) { Say 'nothing to do: give -List, -Revert or -Width and -Height'; exit 2 }

$d = Mode $ENUM_CURRENT
$d.dmPelsWidth = $Width
$d.dmPelsHeight = $Height
$d.dmDisplayFixedOutput = $fixedValue
$d.dmFields = $DM_PELSWIDTH -bor $DM_PELSHEIGHT -bor $DM_DISPLAYFIXEDOUTPUT
try {
    # Flags 0: dynamic only, the registry keeps its mode (DISP_CHANGE_SUCCESSFUL = 0, BADMODE = -2, FAILED = -1).
    $r = [Bc250Modes]::Set($d)
    Say "set ${Width}x${Height} fixed-output $Fixed ($fixedValue): ChangeDisplaySettingsEx = $r; current $(Text (Mode $ENUM_CURRENT))"
    if ($r -eq 0) {
        Say "hold $HoldSeconds s"
        Start-Sleep -Seconds $HoldSeconds
    }
} finally {
    BackToRegistry
}
