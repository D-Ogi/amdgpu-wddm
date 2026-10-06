# r17e repair over r17d: which files the installer found in use and how it handled them (scheduled at restart,
# renamed aside), plus the pending rename list. Reads only.
$c = 'C:\BC250\tmp\clean17\install-console.txt'
Select-String -LiteralPath $c -Pattern 'in use|schedul|aside|\.old|pending|at the next start|locked|MoveFile|replace' | ForEach-Object { $_.Line.Trim() } | Select-Object -First 40
$p = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
'pending rename entries: {0}' -f @($p | Where-Object { $_ }).Count
@($p | Where-Object { $_ -match 'amdgpu|bc250' }) | Select-Object -First 30
Get-ChildItem 'C:\Program Files\amdgpu-wddm' -Recurse -File -Force | Where-Object { $_.Name -match '\.old|\.del|~' } | ForEach-Object { 'aside: ' + $_.FullName }
