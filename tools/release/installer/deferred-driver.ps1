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

# BD-227: INSTALL_AS_SET installs the staged set at shutdown only on a device that it is the best match for, and
# DIIRFLAG_FORCE_INF is ignored together with it (DiInstallDriverW reference), so no deferred install can be forced.
# Two packages of ours have the same signer and hardware ID, so PnP orders them by the DriverVer date and then the
# version, and on a tie it keeps the installed one (setupapi: "Driver does not need to be installed on device"). The
# GPU then stays on the old package, and that KMD refuses the state that this install freezes for its own transaction
# (state_kernel.c Source(): CM_PROB_FAILED_DRIVER_ENTRY after the restart). A forced install binds a live display
# device, which BD-060 forbids under a running desktop. So the install stops before its first change instead.
# Pure: $Bound is a reading of Read-DriverSetBinding. { ok, id, detail }; id is the end of the preflight check id.
function Get-DriverSetRankDecision {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$StagedInfText, [Parameter(Mandatory)][string]$StagedInfSha, $Bound)
    function Out([bool]$Ok, [string]$Id, [string]$Detail) { return [pscustomobject]@{ ok = $Ok; id = $Id; detail = $Detail } }
    try { $staged = Get-InfDriverIdentity $StagedInfText } catch { return Out $false 'unreadable' "the INF of this package: $($_.Exception.Message)" }
    if (-not $Bound -or -not $Bound.ours) {
        return Out $true 'ok' "the GPU uses $(if ($Bound -and $Bound.service) { "the service '$($Bound.service)'" } else { 'no driver service' }), not a package of ours: this package (DriverVer $($staged.date),$($staged.quad)) becomes its driver at the restart"
    }
    $remedy = 'Uninstall the driver (uninstall.cmd), restart, and install this package again.'
    if (-not $Bound.inf -or -not $Bound.inf_sha256 -or $null -eq $Bound.inf_text) {
        return Out $false 'unreadable' "the GPU uses a package of ours, but its INF $(if ($Bound.inf) { "($($Bound.inf)) " })cannot be read, so it is not known whether Windows moves the GPU to this package at the restart. $remedy"
    }
    if (([string]$Bound.inf_sha256).ToUpperInvariant() -eq $StagedInfSha.ToUpperInvariant()) {
        return Out $true 'ok' "the GPU uses this package already ($($Bound.inf), DriverVer $($staged.date),$($staged.quad))"
    }
    try { $installed = Get-InfDriverIdentity ([string]$Bound.inf_text) } catch { return Out $false 'unreadable' "the INF of the installed package $($Bound.inf): $($_.Exception.Message). $remedy" }
    $sd = ConvertTo-DriverDate $staged.date; $bd = ConvertTo-DriverDate $installed.date
    $above = ($sd -gt $bd) -or ($sd -eq $bd -and (ConvertTo-DriverQuad $staged.quad) -gt (ConvertTo-DriverQuad $installed.quad))
    if ($above) { return Out $true 'ok' "this package (DriverVer $($staged.date),$($staged.quad)) ranks above the installed $($Bound.inf) ($($installed.date),$($installed.quad))" }
    return Out $false 'not-above' ("the GPU uses $($Bound.inf) with DriverVer $($installed.date),$($installed.quad), and this package has DriverVer $($staged.date),$($staged.quad), " +
        'which does not rank above it. Windows would keep the GPU on the installed package at the restart, and that driver would not start ' +
        "with the settings that this install hands over (BD-227). $remedy")
}

# The package of ours that the GPU uses now, for Get-DriverSetRankDecision: the device's service, and when it is ours
# the published INF (oem<n>.inf, a byte copy of the driver-store INF) with its SHA256 and text. Read only.
function Read-DriverSetBinding {
    param([Parameter(Mandatory)][string]$InstanceId, [Parameter(Mandatory)][string]$ServiceName)
    $svc = Get-DeviceServiceName -InstanceId $InstanceId
    $r = [ordered]@{ service = $svc; ours = ([string]$svc -ieq $ServiceName); inf = $null; inf_sha256 = $null; inf_text = $null }
    if ($r.ours) {
        $name = Get-DeviceDriverPackage -InstanceId $InstanceId
        $r.inf = $name
        if ($name -match '\Aoem[0-9]+\.inf\z') {
            $path = Join-Path (Join-Path $env:windir 'INF') $name
            if (Test-Path -LiteralPath $path -PathType Leaf) { $r.inf_sha256 = Get-Sha256 $path; $r.inf_text = [IO.File]::ReadAllText($path) }
        }
    }
    return [pscustomobject]$r
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
