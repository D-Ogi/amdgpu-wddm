# The Vulkan memory heaps of the INSTALLED system ICD, beside the kernel driver's own segment geometry.
# Read-only except for its output directory under C:\BC250\tmp. Generic: it names no train and no package.
#
# Why this arm exists. Until 0.7.216.100-tester.27 the private caps blob carried the memory sizes of the
# machine the driver was first measured on, so a Vulkan heap did not follow the board's carve-out. The caps
# now carry the application segment's size, and this arm is what reads the result: the DEVICE_LOCAL heap of
# the installed ICD against the segment the kernel driver publishes. Run it at both carve-outs (8192 and
# 12288 MiB) and the two readings must differ by about the 4096 MiB the board moved.
#
#   vulkaninfo   the release's own tools\vulkaninfo.exe through the registered loader entry, full output,
#                parsed for VkPhysicalDeviceMemoryProperties
#   vram         bc250kmd_cli vram, the dxgkrnl segment statistics of the adapter
# One call each; the whole script stays well under the three-minute bound.
param([string]$Out = 'C:\BC250\tmp\train-vkheaps')
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$out = $Out
New-Item -ItemType Directory -Force $out | Out-Null
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
function Sha8($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 8) } else { 'ABSENT' } }
"vkheaps start $([DateTime]::UtcNow.ToString('o'))"
"system ICD   $(Sha8 (Join-Path $inst 'vulkan\vulkan_radeon.dll'))"
"vulkaninfo   $(Sha8 (Join-Path $inst 'tools\vulkaninfo.exe'))"

$vi = Join-Path $inst 'tools\vulkaninfo.exe'
$o = "$out\vulkaninfo-full.txt"
$e = "$out\vulkaninfo-full.err"
Remove-Item Env:\VK_DRIVER_FILES -ErrorAction SilentlyContinue
Remove-Item Env:\VK_ICD_FILENAMES -ErrorAction SilentlyContinue
$s = Get-Date
$p = Start-Process -FilePath $vi -Wait -PassThru -NoNewWindow -RedirectStandardOutput $o -RedirectStandardError $e
'vulkaninfo exit {0} after {1:N1} s' -f $p.ExitCode, ((Get-Date) - $s).TotalSeconds
$err = @(Get-Content $e -ErrorAction SilentlyContinue)
if ($err.Count) { '  stderr: ' + ($err | Select-Object -First 3) }

# The heap table of VkPhysicalDeviceMemoryProperties: one memoryHeaps[N] block, its size in bytes, and the
# flags under it. The first DEVICE_LOCAL heap is the local one an application allocates its images in.
$lines = @(Get-Content -LiteralPath $o -ErrorAction SilentlyContinue)
$heaps = @()
$index = -1
foreach ($line in $lines) {
    if ($line -match 'memoryHeaps\[(\d+)\]') {
        $index = [int]$Matches[1]
        while ($heaps.Count -le $index) { $heaps += [pscustomobject]@{ Bytes = [UInt64]0; DeviceLocal = $false } }
        continue
    }
    if ($index -lt 0) { continue }
    if ($line -match '^\s*size\s*=\s*(\d+)') { if ($heaps[$index].Bytes -eq 0) { $heaps[$index].Bytes = [UInt64]$Matches[1] } ; continue }
    if ($line -match 'MEMORY_HEAP_DEVICE_LOCAL_BIT') { $heaps[$index].DeviceLocal = $true ; continue }
    if ($line -match 'memoryTypes|^\s*$') { $index = -1 }
}
$local = @($heaps | Where-Object DeviceLocal)
$host_ = @($heaps | Where-Object { -not $_.DeviceLocal })
$localMb = if ($local.Count) { [int]([Math]::Round($local[0].Bytes / 1MB)) } else { 0 }
$hostMb = if ($host_.Count) { [int]([Math]::Round($host_[0].Bytes / 1MB)) } else { 0 }
$totalMb = [int]([Math]::Round((($heaps | Measure-Object -Property Bytes -Sum).Sum) / 1MB))
'heaps read: {0}' -f $heaps.Count
foreach ($h in $heaps) { '  {0,8} MiB  device_local={1}' -f [int]([Math]::Round($h.Bytes / 1MB)), $h.DeviceLocal }
'vk heaps: device-local {0} MiB, host {1} MiB, total {2} MiB' -f $localMb, $hostMb, $totalMb

'--- bc250kmd_cli vram'
$vram = & $cli vram 2>&1
$vram | Out-File -Encoding utf8 "$out\vram.txt"
$vram | Select-Object -First 12 | ForEach-Object { '  ' + "$_" }
'vkheaps done'
