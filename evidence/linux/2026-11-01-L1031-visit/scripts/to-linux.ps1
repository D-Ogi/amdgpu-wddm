# Steer unit A to the diagnostic stick's Linux (GRUB default 4: network only, amdgpu blacklisted) and restart.
# Identity check first: label BC250DIAG, FAT32, USB bus, about 32 GB. Only the loader name changes; no firmware entry.
$ErrorActionPreference = 'Stop'
$v = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.FileSystemType -eq 'FAT32' }
if (@($v).Count -ne 1) { throw "stick not found exactly once" }
$disk = Get-Disk -Number (Get-Partition -DriveLetter $v.DriveLetter).DiskNumber
if ($disk.BusType -ne 'USB' -or $v.Size -lt 28GB -or $v.Size -gt 34GB) { throw "stick identity mismatch: $($disk.BusType) $($v.Size)" }
$d = "$($v.DriveLetter):"
$cfg = Get-Content "$d\boot\grub\grub.cfg" | Select-String '^set default=4$'
if (-not $cfg) { throw 'grub default is not 4' }
$off = "$d\efi\boot\bootx64.off"; $efi = "$d\efi\boot\bootx64.efi"
if (Test-Path $efi) { 'loader already bootx64.efi' } elseif (Test-Path $off) { Rename-Item $off 'bootx64.efi'; 'loader renamed off -> efi' } else { throw 'no loader file' }
Get-ChildItem "$d\efi\boot" | ForEach-Object { '  ' + $_.Name }
'restart at ' + (Get-Date).ToUniversalTime().ToString('s') + 'Z'
shutdown.exe /r /t 15 /c "BC-250 lab: switching to the Linux diagnostic stick"
