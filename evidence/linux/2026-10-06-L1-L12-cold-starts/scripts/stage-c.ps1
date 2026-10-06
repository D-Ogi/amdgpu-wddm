# Copy the L1+L12 cold-start kit from C:\BC250\tmp\stick-c onto the positively identified diagnostic stick (\l1006c).
$ErrorActionPreference = 'Stop'
$v = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.FileSystemType -eq 'FAT32' }
if (@($v).Count -ne 1) { throw "stick not found exactly once" }
$disk = Get-Disk -Number (Get-Partition -DriveLetter $v.DriveLetter).DiskNumber
if ($disk.BusType -ne 'USB' -or $v.Size -lt 28GB -or $v.Size -gt 34GB) { throw "stick identity mismatch: $($disk.BusType) $($v.Size)" }
$d = "$($v.DriveLetter):"
$dst = "$d\l1006c"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item -Recurse -Force C:\BC250\tmp\stick-c\* $dst
"files on stick: " + (Get-ChildItem -Recurse -File $dst).Count
"free MB: " + [int]($v.SizeRemaining / 1MB)
Get-ChildItem "$d\efi\boot" | ForEach-Object { 'loader ' + $_.Name }
