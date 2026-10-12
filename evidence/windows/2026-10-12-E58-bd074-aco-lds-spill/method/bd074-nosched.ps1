# BD-074 lab check 1 (ANALYSIS.md section 7): ray_query.misc.dynamic_indexing under rtwave64 with the ACO pre-RA
# scheduler off. Pass means the reordering of LDS spill reloads by the scheduler is the cause. One RunId, 120 s bound.
$env:RADV_DEBUG = 'nocache'
$env:ACO_DEBUG = 'nosched'
$common = @{ CaseList = 'C:\BC250\cts\batches\rt-k97-di1.txt'; Route = 'direct'
             IcdPath = 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll'; BoundSeconds = 120 }
& 'C:\BC250\cts\run-batch.ps1' @common -RunId 'bd074nosched-w64-t28' -Perftest 'rtwave64' 2>&1 | Select-Object -Last 6
Get-ChildItem -LiteralPath 'C:\BC250\cts\results\bd074nosched-w64-t28' -Recurse -File -ErrorAction SilentlyContinue |
    ForEach-Object { "{0} {1} bytes" -f $_.FullName, $_.Length }
