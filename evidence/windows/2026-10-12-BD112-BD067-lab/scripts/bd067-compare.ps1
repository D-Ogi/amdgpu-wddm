# BD-067: compare the session 0 and session 1 vulkaninfo outputs of bd067-vulkaninfo.ps1. Read only.
$dir = 'C:\BC250\tmp\bd067'
$a = Get-Content -LiteralPath "$dir\s1.txt"
$b = Get-Content -LiteralPath "$dir\s0.txt"
"s1 deviceName: $(($a | Select-String 'deviceName' | Select-Object -First 1).Line.Trim())"
"s0 deviceName: $(($b | Select-String 'deviceName' | Select-Object -First 1).Line.Trim())"
"s0 'Device Properties and Extensions' sections: $(($b | Select-String 'Device Properties and Extensions').Count)"
"s0 VK_KHR_swapchain lines: $(($b | Select-String 'VK_KHR_swapchain').Count), s1: $(($a | Select-String 'VK_KHR_swapchain').Count)"
'=== lines only in session 1 (first 12)'
Compare-Object -ReferenceObject $a -DifferenceObject $b | Where-Object SideIndicator -eq '<=' | Select-Object -First 12 | ForEach-Object { $_.InputObject }
'=== lines only in session 0 (first 12)'
Compare-Object -ReferenceObject $a -DifferenceObject $b | Where-Object SideIndicator -eq '=>' | Select-Object -First 12 | ForEach-Object { $_.InputObject }
'=== s0.err'
Get-Content -LiteralPath "$dir\s0.err" -ErrorAction SilentlyContinue | Select-Object -First 10
