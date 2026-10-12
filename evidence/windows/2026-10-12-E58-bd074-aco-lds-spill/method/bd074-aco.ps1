# BD-074: the ACO disassembly of ray_query.misc.dynamic_indexing under the default wave size and under rtwave64.
# Runs the pinned CTS runner with RADV_DEBUG=shaders,nocache inherited from this process; the dump goes to the
# attempt's stderr file. Two new RunIds, each bounded to 120 s.
param([string]$Mode = 'both')
$env:RADV_DEBUG = 'shaders,nocache'
$common = @{ CaseList = 'C:\BC250\cts\batches\rt-k97-di1.txt'; Route = 'direct'
             IcdPath = 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll'; BoundSeconds = 120 }
if ($Mode -in 'both', 'def') { & 'C:\BC250\cts\run-batch.ps1' @common -RunId 'bd074acodef-t28' 2>&1 | Select-Object -Last 4 }
if ($Mode -in 'both', 'w64') { & 'C:\BC250\cts\run-batch.ps1' @common -RunId 'bd074acow64-t28' -Perftest 'rtwave64' 2>&1 | Select-Object -Last 4 }
foreach ($r in 'bd074acodef-t28', 'bd074acow64-t28') {
    Get-ChildItem -LiteralPath "C:\BC250\cts\results\$r" -Recurse -Filter 'attempt-*.std*.txt' -ErrorAction SilentlyContinue |
        ForEach-Object { "{0} {1} bytes" -f $_.FullName, $_.Length }
}
