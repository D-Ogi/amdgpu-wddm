# BD-152: stage a driver set without rebinding a live display device (BD-060).
# DiInstallDriverW / DIIRFLAG_INSTALL_AS_SET, SDK 26100 newdev.h and the Microsoft API reference:
# all INFs in the directory are staged now, activated and installed at the next shutdown.
# PowerShell 5.1. Dot-sourcing defines functions only; host tests inject the native call.

function Test-InfHasReboot([string[]]$Lines) {
    return [bool](@($Lines | Where-Object { ($_ -replace ';.*$', '').Trim() -match '^Reboot(?:\s|=|$)' }).Count)
}

function Invoke-DriverSetStage {
    param([Parameter(Mandatory)][string]$InfFile, [scriptblock]$NativeCall)
    $inf = [IO.Path]::GetFullPath($InfFile)
    $dir = Split-Path -Parent $inf
    $files = @(Get-ChildItem -LiteralPath $dir -Filter '*.inf' -File)
    # INSTALL_AS_SET takes a directory, not an INF. An extra INF must never slip into that set.
    if ($files.Count -ne 1 -or $files[0].FullName -ine $inf) { throw 'driver set must contain exactly the selected KMD INF' }
    if (Test-InfHasReboot ([IO.File]::ReadAllLines($inf))) { throw 'Reboot is not supported in a display INF (BD-152)' }
    if (-not $NativeCall) {
        if (-not ('AmdgpuWddm.DeferredDriver' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace AmdgpuWddm {
    public static class DeferredDriver {
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("newdev.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool DiInstallDriverW(IntPtr parent, string directory, uint flags,
            [MarshalAs(UnmanagedType.Bool)] out bool needReboot);
        public static bool Stage(string directory, uint flags) {
            bool reboot;
            if (!DiInstallDriverW(IntPtr.Zero, directory, flags, out reboot))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return reboot;
        }
    }
}
'@
        }
        $NativeCall = { param($Directory, $Flags) [AmdgpuWddm.DeferredDriver]::Stage($Directory, $Flags) }
    }
    $needReboot = & $NativeCall $dir ([uint32]0x40) # DIIRFLAG_INSTALL_AS_SET; never FORCE_INF
    if ($needReboot -isnot [bool]) { throw 'driver set API did not return its BOOL restart result' }
    # Even FALSE says nothing about the current binding. INSTALL_AS_SET still activates at shutdown.
    return [pscustomobject]@{ method = 'di-install-as-set'; deferred = $true; api_need_reboot = $needReboot
        text = "driver set staged; activation at shutdown; API NeedReboot=$needReboot" }
}

function Get-DriverSetAction {
    param($Receipt, [Parameter(Mandatory)][string]$InfSha, [AllowNull()][string]$BootId)
    if (-not $BootId) { return 'unknown-boot' }
    if (-not $Receipt -or [string]$Receipt.inf_sha256 -ne $InfSha) { return 'stage' }
    if ([string]$Receipt.method -ne 'di-install-as-set' -or -not $Receipt.boot_id -or
        $Receipt.staged -isnot [bool] -or -not $Receipt.staged) { return 'invalid-receipt' }
    if ([string]$Receipt.boot_id -eq $BootId) { return 'restart' }
    return 'verify-binding'
}
