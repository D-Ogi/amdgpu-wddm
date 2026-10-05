# Read-only defaults audit probe (registry and file listings only; no writes, no launches).
$ErrorActionPreference = 'Continue'
function Show-Key([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { "[$Path] absent"; return }
    "[$Path]"
    $k = Get-Item -LiteralPath $Path
    foreach ($n in $k.GetValueNames()) {
        $v = $k.GetValue($n, $null, 'DoNotExpandEnvironmentNames')
        if ($v -is [array]) { $v = ($v -join ' | ') }
        "  $n ($($k.GetValueKind($n))) = $v"
    }
}
function Show-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { "[$Path] absent"; return }
    Show-Key $Path
    foreach ($c in Get-ChildItem -LiteralPath $Path -Recurse -ErrorAction SilentlyContinue) { Show-Key $c.PSPath.Replace('Microsoft.PowerShell.Core\Registry::', 'Registry::') }
}
"=== display class key of the BC-250"
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
foreach ($d in $dev) {
    "device status $($d.Status) class $($d.Class)"
    $drvKey = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Driver).Data
    $p = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drvKey"
    $k = Get-Item -LiteralPath $p
    foreach ($n in @('UserModeDriverName','UserModeDriverNameWow','UserModeDListDriverName','OpenGLDriverName','OpenGLDriverNameWow','OpenGLVersion','OpenGLFlags','VulkanDriverName','VulkanDriverNameWow','VulkanImplicitLayers','OpenCLDriverName','OpenCLDriverNameWow','AmdgpuWddmSparseBinding','DriverVersion','FeatureScore','HardwareInformation.qwMemorySize','HardwareInformation.MemorySize')) {
        $v = $k.GetValue($n, $null)
        if ($null -eq $v) { "  $n = <absent>" } else { if ($v -is [array]) { $v = ($v -join ' | ') }; "  $n ($($k.GetValueKind($n))) = $v" }
    }
    "  other value names: " + (($k.GetValueNames() | Where-Object { $_ -notmatch '^(UserModeDriverName|VulkanDriverName|DriverDesc|ProviderName|DriverDate|DriverDateData|InfPath|InfSection|MatchingDeviceId)' }) -join ', ')
}
"=== amdgpu-wddm policy"
Show-Tree 'HKLM:\SOFTWARE\amdgpu-wddm'
"=== Khronos"
Show-Key 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
Show-Key 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers'
Show-Key 'HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers'
Show-Key 'HKLM:\SOFTWARE\Khronos\OpenCL\Vendors'
Show-Key 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenCL\Vendors'
"=== KMD parameters (selected)"
$pk = Get-Item -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
foreach ($n in ($pk.GetValueNames() | Where-Object { $_ -match '^(Dpm|CuMode|Enable|Thermal|Keep|Interop|Unconfirmed)' } | Sort-Object)) { "  $n = $($pk.GetValue($n))" }
"=== GraphicsDrivers (HAGS etc.)"
$gd = Get-Item -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers'
foreach ($n in @('HwSchMode','TdrDelay','TdrLevel','TdrDebugMode','PlatformSupportMiracast')) { "  $n = $($gd.GetValue($n, '<absent>'))" }
"=== Our DLLs in SysWOW64 / System32"
foreach ($f in @('bc250umd.dll','bc250d3d_router.dll','amdgpu_wddm_d3d12.dll','amdgpu_wddm_d3d11.dll','vulkan-1.dll','opengl32.dll','OpenCL.dll')) {
    foreach ($dir in @("$env:windir\System32", "$env:windir\SysWOW64")) {
        $p = Join-Path $dir $f
        if (Test-Path -LiteralPath $p) { "  $p $((Get-Item $p).Length) bytes" } else { "  $p absent" }
    }
}
"=== Hardware MFTs (video encode/decode) by enumeration flags"
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class MftEnum {
  [DllImport("mfplat.dll")] public static extern int MFStartup(uint ver, uint flags);
  [DllImport("mfplat.dll")] public static extern int MFTEnumEx(Guid cat, uint flags, IntPtr inType, IntPtr outType, out IntPtr pppMFTActivate, out uint count);
  [DllImport("ole32.dll")] public static extern void CoTaskMemFree(IntPtr p);
}
'@
[void][MftEnum]::MFStartup(0x20070, 0)
$cats = @{ 'VIDEO_ENCODER' = [Guid]'f79eac7d-e545-4387-bdee-d647d7bde42a'; 'VIDEO_DECODER' = [Guid]'d6c02d4b-6833-45b4-971a-05a4b04bab91'; 'VIDEO_PROCESSOR' = [Guid]'302ea3fc-aa5f-47f9-9f7a-c2188bb16302' }
foreach ($c in $cats.Keys) {
    $p = [IntPtr]::Zero; $n = 0
    $hr = [MftEnum]::MFTEnumEx($cats[$c], 0x4 -bor 0x10, [IntPtr]::Zero, [IntPtr]::Zero, [ref]$p, [ref]$n)  # HARDWARE | SORTANDFILTER
    "  $c hardware MFTs: hr 0x{0:X8}, count {1}" -f $hr, $n
    if ($p -ne [IntPtr]::Zero) { [MftEnum]::CoTaskMemFree($p) }
}
"=== Display: video controller and monitors"
Get-CimInstance Win32_VideoController | ForEach-Object { "  $($_.Name): $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution) @ $($_.CurrentRefreshRate) Hz, $($_.CurrentBitsPerPixel) bpp, driver $($_.DriverVersion), status $($_.Status)" }
"  monitors (WmiMonitorID count): " + @(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue).Count
Add-Type -AssemblyName System.Windows.Forms
"  screens: " + [System.Windows.Forms.Screen]::AllScreens.Count
"=== Installed release"
Show-Key 'HKLM:\SOFTWARE\amdgpu-wddm\Release' | Where-Object { $_ -notmatch 'AppliedDefaults' }
"=== Scheduled task / power plan"
(powercfg /getactivescheme) 2>&1
