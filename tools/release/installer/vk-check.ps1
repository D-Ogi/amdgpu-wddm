# Vulkan check of the installer's verify phase, run in its own powershell.exe so that a failing ICD cannot end the
# installer. Creates a Vulkan 1.0 instance through the system loader (vulkan-1.dll), lists the physical devices
# and prints one line per device: "device 0x<vendor>:0x<device> <name>". Prints "SKIP ..." when Windows has no
# Vulkan loader (games ship their own copy; nothing to check then) and "ERROR ..." on a failed call.
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath (Join-Path $env:windir 'System32\vulkan-1.dll'))) { 'SKIP: no Vulkan loader (System32\vulkan-1.dll) on this computer'; exit 0 }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AmdgpuWddmVk {
    [StructLayout(LayoutKind.Sequential)]
    public struct InstanceCreateInfo {
        public int sType; public IntPtr pNext; public uint flags; public IntPtr pApplicationInfo;
        public uint enabledLayerCount; public IntPtr ppEnabledLayerNames;
        public uint enabledExtensionCount; public IntPtr ppEnabledExtensionNames;
    }
    [DllImport("vulkan-1.dll")] public static extern int vkCreateInstance(ref InstanceCreateInfo info, IntPtr allocator, out IntPtr instance);
    [DllImport("vulkan-1.dll")] public static extern int vkEnumeratePhysicalDevices(IntPtr instance, ref uint count, [Out] IntPtr[] devices);
    [DllImport("vulkan-1.dll")] public static extern void vkGetPhysicalDeviceProperties(IntPtr device, IntPtr properties);
    [DllImport("vulkan-1.dll")] public static extern void vkDestroyInstance(IntPtr instance, IntPtr allocator);
}
'@
$info = New-Object AmdgpuWddmVk+InstanceCreateInfo
$info.sType = 1   # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
$inst = [IntPtr]::Zero
$r = [AmdgpuWddmVk]::vkCreateInstance([ref]$info, [IntPtr]::Zero, [ref]$inst)
if ($r -ne 0) { "ERROR: vkCreateInstance returned $r"; exit 1 }
try {
    [uint32]$n = 0
    $r = [AmdgpuWddmVk]::vkEnumeratePhysicalDevices($inst, [ref]$n, $null)
    if ($r -ne 0) { "ERROR: vkEnumeratePhysicalDevices returned $r"; exit 1 }
    if ($n -eq 0) { 'ERROR: the loader lists no Vulkan device'; exit 1 }
    $devs = New-Object IntPtr[] $n
    $r = [AmdgpuWddmVk]::vkEnumeratePhysicalDevices($inst, [ref]$n, $devs)
    if ($r -ne 0 -and $r -ne 5) { "ERROR: vkEnumeratePhysicalDevices returned $r"; exit 1 }
    # VkPhysicalDeviceProperties: apiVersion @0, driverVersion @4, vendorID @8, deviceID @12, deviceType @16,
    # deviceName char[256] @20. 1024 bytes cover the whole structure.
    $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(1024)
    try {
        foreach ($d in $devs) {
            [AmdgpuWddmVk]::vkGetPhysicalDeviceProperties($d, $buf)
            $vendor = [Runtime.InteropServices.Marshal]::ReadInt32($buf, 8)
            $device = [Runtime.InteropServices.Marshal]::ReadInt32($buf, 12)
            $api = [Runtime.InteropServices.Marshal]::ReadInt32($buf, 0)
            $name = [Runtime.InteropServices.Marshal]::PtrToStringAnsi([IntPtr]::Add($buf, 20))
            'device 0x{0:X4}:0x{1:X4} {2} (Vulkan {3}.{4})' -f $vendor, $device, $name, (($api -shr 22) -band 0x7F), (($api -shr 12) -band 0x3FF)
        }
    } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
} finally { [AmdgpuWddmVk]::vkDestroyInstance($inst, [IntPtr]::Zero) }
exit 0
