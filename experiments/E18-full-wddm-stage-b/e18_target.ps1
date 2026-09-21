# E18 on the target (bc250kmd 0.7.10+, M7 stage B; E16's script plus -GpuVa), one phase per call,
# everything logged under <Package>\out. Runs elevated (SSH session). No GPU register is touched by any phase here
# except `sweep`, which is the read-only witness.
#
#   -Phase install                     pnputil the package in -Package (every install closes every gate)
#   -Phase unumd                       after run 2: remove its package from the store, which returns the plain one
#   -Phase gate -Full 0|1            EnableMmio, EnableVram and EnableFullWddm = Full, every engine gate 0, then a
#                                      device disable/enable so that DriverEntry reads the gate again
#   -Phase state -Tag <t>              device, driver, breadcrumbs, gates, video controller, event and report COUNTS
#   -Phase log -Tag <t>                the driver's log ring (read it BEFORE the gate is closed: an unload takes it along)
#   -Phase confirm                     clear UnconfirmedStarts (only after `state` looked healthy)
#   -Phase d3d -Tag <t>                D3D11CreateDevice on the hardware adapter and on WARP; the HRESULTs are the data
#   -Phase sweep -Tag <t>              witness sweeps
param(
    [Parameter(Mandatory)][ValidateSet('install', 'unumd', 'gate', 'state', 'log', 'confirm', 'd3d', 'sweep')][string]$Phase,
    [string]$Tag = 'x',
    [int]$Full = 0,
    [int]$GpuVa = 0,                    # with -Phase gate: EnableGpuVa and EnableVramWrite (stage B writes)
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
    Say ("driver   {0}   ({1})" -f (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data,
        (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data)
    # Which run this is, read from where dxgkrnl reads it: the run 2 package registers a user-mode driver, the plain
    # one does not. A run 2 without this value is run 1 under another name.
    $class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
    $umd = (Get-ItemProperty $class -Name UserModeDriverName -ErrorAction SilentlyContinue).UserModeDriverName
    Say ("umd      UserModeDriverName {0}" -f $(if ($umd) { $umd -join ' | ' } else { 'absent (plain package)' }))
    Say ("stages   last {0}   history {1}   unconfirmed {2}" -f $p.LastStage, $p.StageHistory, $p.UnconfirmedStarts)
    Say ("keeplog  KeepLog {0}  KeepStatus {1}  files {2}" -f $p.KeepLog, $(if ($null -ne $p.KeepStatus) { '0x{0:X8}' -f $p.KeepStatus } else { 'never' }),
        @(Get-ChildItem 'C:\BC250\kmdlog' -File -ErrorAction SilentlyContinue).Count)
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
    'unumd' {
        # The way back from run 2. Its package is one build number above the plain one (so that it wins the install
        # instead of tying with it), which also means the plain package cannot be installed over it. Removing the
        # run 2 package from the store hands the device to the best package left, the plain 0.7.x of run 1.
        $gpu = Gpu
        $inf = (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
        $class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
        if (-not (Get-ItemProperty $class -Name UserModeDriverName -ErrorAction SilentlyContinue)) {
            Say "unumd: the installed package ($inf) registers no user-mode driver; nothing removed"
        } elseif ($inf -notmatch '^oem\d+\.inf$') {
            Say "unumd: unexpected INF name '$inf'; nothing removed"
        } else {
            # Without a plain package left in the store the device would fall to Basic Display and take the escape,
            # the ring and this tool's counterpart with it: that would read as a failed experiment, not as a missing
            # package. Get-WindowsDriver rather than pnputil's text, which is localized.
            $others = @(Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\bc250kmd.inf' -and $_.Driver -ne $inf })
            Say ("unumd: other bc250kmd packages in the store: " + (($others | ForEach-Object { "$($_.Driver) $($_.Version)" }) -join ', '))
            if ($others.Count -eq 0) {
                Say "unumd: no plain package in the store; nothing removed (install the plain package first)"
            } else {
                pnputil /delete-driver $inf /uninstall 2>&1 | ForEach-Object { Say "$_" }
                Start-Sleep -Seconds 10
                Say ("unumd: bc250umd.dll in System32 after the uninstall: " + (Test-Path 'C:\Windows\System32\bc250umd.dll'))
                # The restart this caused counts against the start budget like any other.
                Say "unumd: read 'unconfirmed' below; run -Phase confirm if it is not 0 and the state is healthy"
            }
        }
        State
        & $cli info 2>&1 | ForEach-Object { Say "$_" }
    }
    'gate' {
        foreach ($name in 'EnableMmioWrite', 'EnableGart', 'EnablePsp', 'EnableGfx', 'EnableIh') { Set-ItemProperty $params -Name $name -Value 0 -Type DWord }
        Set-ItemProperty $params -Name EnableMmio -Value $Full -Type DWord
        Set-ItemProperty $params -Name EnableVram -Value $Full -Type DWord
        $va = if ($Full -eq 1) { $GpuVa } else { 0 }
        Set-ItemProperty $params -Name EnableVramWrite -Value $va -Type DWord
        Set-ItemProperty $params -Name EnableGpuVa -Value $va -Type DWord
        Say "gate     EnableGpuVa $va  EnableVramWrite $va"
        Set-ItemProperty $params -Name EnableFullWddm -Value $Full -Type DWord
        # 0.7.3: the ring goes into C:\BC250\kmdlog at every stop while the gate is open, because dxgkrnl may end a
        # full WDDM start by itself and unload the driver, ring and all (run 1 with 0.7.2 did exactly that).
        Set-ItemProperty $params -Name KeepLog -Value $Full -Type DWord
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
