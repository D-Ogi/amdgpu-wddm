# Zip the L1+L12 outputs from the positively identified stick (\l1006c\out and the run logs) for pulling.
$ErrorActionPreference = 'Stop'
$v = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.FileSystemType -eq 'FAT32' }
if (@($v).Count -ne 1) { throw "stick not found exactly once" }
$d = "$($v.DriveLetter):"
$src = "$d\l1006c"
$zip = 'C:\BC250\tmp\l1006c-out.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$src\out", "$src\run-*.log" -DestinationPath $zip
(Get-Item $zip).Length
Get-ChildItem "$d\efi\boot" | ForEach-Object { 'loader ' + $_.Name }
