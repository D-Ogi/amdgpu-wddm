$ErrorActionPreference='Stop'
$volume=Get-Volume -DriveLetter D
$disk=Get-Partition -DriveLetter D | Get-Disk
if($volume.FileSystemLabel -ne 'BC250DIAG' -or $volume.FileSystem -ne 'FAT32' -or $disk.BusType -ne 'USB' -or $disk.Size -ne 64160400896){throw 'Diagnostic USB identity mismatch'}
'usb_identity_verified=true'
foreach($rel in @('boot\grub\grub.cfg','efi\boot\bootx64.off','bc250diag.apkovl.tar.gz')){
 $p=Join-Path 'D:\' $rel
 if(-not(Test-Path -LiteralPath $p)){throw ('Missing '+$rel)}
 'file='+$rel+' sha256='+(Get-FileHash -LiteralPath $p).Hash
}
Get-Content -LiteralPath 'D:\boot\grub\grub.cfg'
'loader_active='+(Test-Path -LiteralPath 'D:\efi\boot\bootx64.efi')
Get-ChildItem -LiteralPath 'D:\apks\x86_64' -Filter '*flashrom*' -ErrorAction SilentlyContinue | ForEach-Object {$_.Name}
Get-ChildItem -LiteralPath 'D:\bc250' -Filter '*flashrom*' -ErrorAction SilentlyContinue | ForEach-Object {$_.Name}
'usb_inspection_complete'
