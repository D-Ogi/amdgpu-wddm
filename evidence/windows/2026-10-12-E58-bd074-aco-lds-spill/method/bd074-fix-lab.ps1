# BD-074 lab checks 2 and 3 (ANALYSIS.md section 7) with the patched ICD E738ECE1 (b30 source + proposed.patch):
# 2) ray_query.misc.dynamic_indexing under rtwave64 with RADV_DEBUG=shaders,nocache (ACO dump in the stderr file);
# 3) the 140-case rt-k97 list in the default mode and under rtwave64. Each run bounded to 120 s, about 80 s in all.
$icd = 'C:\BC250\tmp\bd074fix\vulkan_radeon.dll'
"icd $((Get-FileHash -Algorithm SHA256 -LiteralPath $icd).Hash)"
$base = @{ Route = 'direct'; IcdPath = $icd; BoundSeconds = 120 }
$env:RADV_DEBUG = 'shaders,nocache'
& 'C:\BC250\cts\run-batch.ps1' @base -CaseList 'C:\BC250\cts\batches\rt-k97-di1.txt' -RunId 'bd074fix-di1-w64' -Perftest 'rtwave64' 2>&1 | Select-Object -Last 3
Remove-Item Env:\RADV_DEBUG
& 'C:\BC250\cts\run-batch.ps1' @base -CaseList 'C:\BC250\cts\batches\rt-k97.txt' -RunId 'bd074fix-k97-def' 2>&1 | Select-Object -Last 6
& 'C:\BC250\cts\run-batch.ps1' @base -CaseList 'C:\BC250\cts\batches\rt-k97.txt' -RunId 'bd074fix-k97-w64' -Perftest 'rtwave64' 2>&1 | Select-Object -Last 6
Get-ChildItem -LiteralPath 'C:\BC250\cts\results\bd074fix-di1-w64' -Recurse -Filter 'attempt-01.stderr.txt' | ForEach-Object { "{0} {1} bytes" -f $_.FullName, $_.Length }
