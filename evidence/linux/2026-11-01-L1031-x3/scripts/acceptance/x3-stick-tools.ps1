# Copy the deployed X3 Linux tools onto the stick (bc250\tools\x3), replacing the pre-review sources there.
$ErrorActionPreference = 'Stop'
$src = 'C:\BC250\tmp\x3stick'
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
$disk = @(Get-Partition -DriveLetter $v[0].DriveLetter | Get-Disk)
if ($v.Count -ne 1 -or $v[0].FileSystem -ne 'FAT32' -or $disk[0].BusType -ne 'USB' -or $disk[0].Size -ne 64160400896) { throw 'stick identity mismatch' }
$dst = "$($v[0].DriveLetter):\bc250\tools\x3"
if (-not (Test-Path $dst)) { throw "missing $dst" }
'before: ' + ((Get-ChildItem $dst -File | ForEach-Object Name) -join ' ')
foreach ($f in Get-ChildItem $src -File) {
    Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dst $f.Name) -Force
    if ((Get-FileHash $f.FullName).Hash -ne (Get-FileHash (Join-Path $dst $f.Name)).Hash) { throw "readback $($f.Name)" }
}
Get-ChildItem $dst -File | ForEach-Object { '{0} {1}' -f $_.Name, (Get-FileHash $_.FullName).Hash.Substring(0,16) }
$readme = "$($v[0].DriveLetter):\bc250\tools\README"
Add-Content -LiteralPath $readme -Value @(
  '',
  'x3/ replaced 2026-11-01 ~20:05Z by the revision accepted on the lab (RESULT.md X3 section):',
  '  sh /media/usb/bc250/tools/x3/to-windows-x3.sh   leave Linux through X3 (default 6, counter cleared, reboot)',
  '  sh /media/usb/bc250/tools/x3/x3-check.sh        read the X3 state; both need python3, the first also grub (apk add grub)',
  '  x3-install.sh only upgrades the external grub.cfg; it never writes the loader.')
Remove-Item -LiteralPath $src -Recurse -Force
'done'
