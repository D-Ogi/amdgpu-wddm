# LAB ONLY. Read-only probe in the logged-on user's desktop; no window is created.
# Caller must bound this whole child process to 10 s (DwmFlush has no timeout).
$ErrorActionPreference = 'Stop'
try {
    $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
    if ($session -le 0) { throw 'requires interactive session, not SYSTEM session 0' }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class X3DesktopProbe {
    [DllImport("user32.dll")] static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeout(
        IntPtr window, uint message, UIntPtr wparam, IntPtr lparam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("dwmapi.dll")] static extern int DwmFlush();
    public static uint Read() {
        IntPtr shell = GetShellWindow();
        if (shell == IntPtr.Zero) throw new Exception("no shell window");
        uint pid;
        if (GetWindowThreadProcessId(shell, out pid) == 0 || pid == 0) throw new Exception("no shell owner");
        UIntPtr result;
        // WM_NULL, SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000 ms.
        if (SendMessageTimeout(shell, 0, UIntPtr.Zero, IntPtr.Zero, 0x23, 2000, out result) == IntPtr.Zero)
            throw new Exception("shell message loop did not respond");
        if (DwmFlush() != 0) throw new Exception("DWM flush failed");
        return pid;
    }
}
'@
    $shellPid = [X3DesktopProbe]::Read()
    $shell = Get-Process -Id $shellPid
    $dwm = @(Get-Process -Name dwm | Where-Object { $_.SessionId -eq $session })
    if ($shell.SessionId -ne $session -or $shell.Path -ine "$env:WINDIR\explorer.exe" -or
        $dwm.Count -ne 1 -or $dwm[0].Path -ine "$env:WINDIR\System32\dwm.exe") { throw 'unexpected shell/DWM identity' }
    $modules = @($dwm[0].Modules | ForEach-Object { $_.ModuleName })
    if ($modules.Count -eq 0) { throw 'DWM module enumeration failed' }
    $router = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter' -Name DwmForceCpu
    $forceCpu = [int]$router.DwmForceCpu
    if ($forceCpu -notin @(0,1)) { throw 'unknown desktop route policy' }
    $zink = $modules -contains 'bc250d3d_zink.dll'
    if (($forceCpu -eq 1 -and $zink) -or ($forceCpu -eq 0 -and -not $zink)) { throw 'actual DWM modules do not match requested route' }
    if ($forceCpu -eq 1 -and -not ($modules -contains 'd3d10warp.dll' -or $modules -contains 'bc250d3d.dll')) {
        throw 'CPU DWM renderer not observed'
    }
    $boot = (Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 5).LastBootUpTime.ToUniversalTime()
    if ($shell.StartTime.ToUniversalTime() -lt $boot -or $dwm[0].StartTime.ToUniversalTime() -lt $boot) { throw 'process identity predates this boot' }
    [pscustomobject]@{ Schema=1; BootUtc=$boot.ToString('o'); SessionId=$session;
        ShellPid=$shellPid; ShellStart=$shell.StartTime.ToUniversalTime().Ticks;
        DwmPid=$dwm[0].Id; DwmStart=$dwm[0].StartTime.ToUniversalTime().Ticks;
        ForceCpu=$forceCpu; Zink=[bool]$zink; CpuRenderer=($modules -contains 'd3d10warp.dll' -or $modules -contains 'bc250d3d.dll');
        ShellResponsive=$true; DwmFlushed=$true } | ConvertTo-Json -Compress
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
