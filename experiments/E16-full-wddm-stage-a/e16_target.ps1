# E16 on the target (bc250kmd 0.7.x, M7 stage A: the full WDDM table behind EnableFullWddm), one phase per call,
# everything logged under <Package>\out. Runs elevated (SSH session). No GPU register is touched by any phase here
# except `sweep`, which is the read-only witness.
#
#   -Phase install                     pnputil the package in -Package (every install closes every gate)
#   -Phase gate -Full 0|1              EnableMmio, EnableVram and EnableFullWddm = Full, every engine gate 0, then a
#                                      device disable/enable so that DriverEntry reads the gate again
#   -Phase state -Tag <t>              device, driver, breadcrumbs, gates, video controller, event and report COUNTS
#   -Phase log -Tag <t>                the driver's log ring (read it BEFORE the gate is closed: an unload takes it along)
#   -Phase confirm                     clear UnconfirmedStarts (only after `state` looked healthy)
#   -Phase d3d -Tag <t>                D3D11CreateDevice on the hardware adapter and on WARP; the HRESULTs are the data
#   -Phase sweep -Tag <t>              witness sweeps
param(
    [Parameter(Mandatory)][ValidateSet('install', 'gate', 'state', 'log', 'confirm', 'd3d', 'sweep')][string]$Phase,
    [string]$Tag = 'x',
    [int]$Full = 0,
    [string]$Package = 'C:\BC250\e16'
)

$ErrorActionPreference = 'Continue'
$out = Join-Path $Package 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$stamp = (Get-Date).ToString('HHmmss')
$log = Join-Path $out "$Phase-$Tag-$stamp.txt"
$cli = Join-Path $Package 'bc250kmd_cli.exe'
$rd = 'C:\BC250\bc250rd\bc250rd_cli.exe'
$reglist = 'C:\BC250\bc250rd\reglist.txt'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$hwid = 'PCI\VEN_1002&DEV_13FE'

function Say([string]$text) { $text | Tee-Object -FilePath $log -Append }
function Gpu { Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$hwid*" } | Select-Object -First 1 }
function State {
    $gpu = Gpu
    $p = Get-ItemProperty $params -ErrorAction SilentlyContinue
    $os = Get-CimInstance Win32_OperatingSystem
    Say ("boot     {0}" -f $os.LastBootUpTime.ToString('s'))
    Say ("device   {0}   status {1}   problem {2}" -f $gpu.FriendlyName, $gpu.Status, $gpu.Problem)
    Say ("driver   {0}" -f (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data)
    Say ("stages   last {0}   history {1}   unconfirmed {2}" -f $p.LastStage, $p.StageHistory, $p.UnconfirmedStarts)
    Say ("gates    EnableFullWddm {0}  EnableMmio {1}  EnableVram {2}  EnableGart {3}  EnablePsp {4}  EnableGfx {5}  EnableIh {6}" -f `
        $p.EnableFullWddm, $p.EnableMmio, $p.EnableVram, $p.EnableGart, $p.EnablePsp, $p.EnableGfx, $p.EnableIh)
    Get-CimInstance Win32_VideoController | ForEach-Object {
        Say ("video    {0} | status {1} | availability {2} | mode {3} | ram {4} | dll {5}" -f $_.Name, $_.Status, $_.Availability, $_.VideoModeDescription, $_.AdapterRAM, $_.InstalledDisplayDrivers)
    }
    # Counts only, since this boot. 4101 = display driver stopped responding and recovered; 1001 = bugcheck report.
    $since = $os.LastBootUpTime
    foreach ($q in @(@{ n = 'Display 4101 (TDR)'; f = @{ LogName = 'System'; ProviderName = 'Display'; Id = 4101; StartTime = $since } },
                     @{ n = 'BugCheck 1001'; f = @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting'; Id = 1001; StartTime = $since } },
                     @{ n = 'Kernel-PnP errors'; f = @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-PnP'; Level = 2; StartTime = $since } },
                     @{ n = 'dwm / Dwminit'; f = @{ LogName = 'Application'; ProviderName = 'Dwminit', 'Desktop Window Manager'; StartTime = $since } })) {
        $n = @(Get-WinEvent -FilterHashtable $q.f -ErrorAction SilentlyContinue).Count
        Say ("events   {0,-22} {1}" -f $q.n, $n)
    }
    $reports = @(Get-ChildItem 'C:\Windows\LiveKernelReports' -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $since }).Count
    Say ("reports  live kernel reports written since boot: $reports")
    $dwm = @(Get-Process dwm -ErrorAction SilentlyContinue)
    Say ("dwm      {0} process(es){1}" -f $dwm.Count, $(if ($dwm.Count) { ", started " + ($dwm | ForEach-Object { $_.StartTime.ToString('HH:mm:ss') }) -join ' ' } else { '' }))
}

Say ("time " + (Get-Date).ToString('s') + "  phase $Phase $Tag")
switch ($Phase) {
    'install' {
        pnputil /add-driver (Join-Path $Package 'bc250kmd.inf') /install 2>&1 | ForEach-Object { Say "$_" }
        Start-Sleep -Seconds 8
        State
        & $cli info 2>&1 | ForEach-Object { Say "$_" }
    }
    'gate' {
        foreach ($name in 'EnableMmioWrite', 'EnableVramWrite', 'EnableGart', 'EnablePsp', 'EnableGfx', 'EnableIh') { Set-ItemProperty $params -Name $name -Value 0 -Type DWord }
        Set-ItemProperty $params -Name EnableMmio -Value $Full -Type DWord
        Set-ItemProperty $params -Name EnableVram -Value $Full -Type DWord
        Set-ItemProperty $params -Name EnableFullWddm -Value $Full -Type DWord
        $gpu = Gpu
        Say ("disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        Start-Sleep -Seconds 4
        Say ("enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' '))
        Start-Sleep -Seconds 10
        State
        & $cli info 2>&1 | ForEach-Object { Say "$_" }
    }
    'state' {
        State
        & $cli info 2>&1 | ForEach-Object { Say "$_" }
    }
    'log' {
        $file = Join-Path $out "ring-$Tag-$stamp.log"
        & $cli log > $file 2>&1
        Say ("log exit $LASTEXITCODE, $((Get-Content $file | Measure-Object -Line).Lines) lines -> $file")
    }
    'confirm' {
        & $cli confirm 2>&1 | ForEach-Object { Say "$_" }
    }
    'd3d' {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class E16D3D {
    [DllImport("d3d11.dll")] static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software, uint flags,
        IntPtr levels, uint levelCount, uint sdk, out IntPtr device, out int level, out IntPtr context);
    public static string Try(int driverType) {
        IntPtr dev, ctx; int level;
        int hr = D3D11CreateDevice(IntPtr.Zero, driverType, IntPtr.Zero, 0, IntPtr.Zero, 0, 7, out dev, out level, out ctx);
        if (ctx != IntPtr.Zero) Marshal.Release(ctx);
        if (dev != IntPtr.Zero) Marshal.Release(dev);
        return String.Format("HRESULT 0x{0:X8}, feature level 0x{1:X}", hr, level);
    }
}
'@
        Say ("d3d11    hardware (D3D_DRIVER_TYPE_HARDWARE): " + [E16D3D]::Try(1))
        Say ("d3d11    WARP     (D3D_DRIVER_TYPE_WARP):     " + [E16D3D]::Try(5))
    }
    'sweep' {
        foreach ($ip in 'GC', 'MMHUB', 'MP0', 'NBIO', 'OSSSYS') {
            $file = Join-Path $out "sweep-$ip-$Tag-$stamp.log"
            & $rd sweep $reglist "$ip." > $file 2>&1
            Say ("sweep $ip exit $LASTEXITCODE, $((Get-Content $file | Measure-Object -Line).Lines) lines -> $file")
        }
    }
}
