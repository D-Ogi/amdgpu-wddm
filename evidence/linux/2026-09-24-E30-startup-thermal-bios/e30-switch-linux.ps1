$ErrorActionPreference='Stop'
$volume=Get-Volume -DriveLetter D
$disk=Get-Partition -DriveLetter D | Get-Disk
if($volume.FileSystemLabel -ne 'BC250DIAG' -or $volume.FileSystem -ne 'FAT32' -or $disk.BusType -ne 'USB' -or $disk.Size -ne 64160400896){throw 'Diagnostic USB mismatch'}
$cfg=Get-Content -LiteralPath 'D:\boot\grub\grub.cfg' -Raw
if($cfg -notmatch '(?m)^set default=4\s*$' -or $cfg -notmatch 'bc250.mode=network'){throw 'Network-only entry missing'}
if((Get-FileHash 'D:\boot\grub\grub.cfg').Hash -ne '380176A8F3AA952BA4CD2FB001F3B365A460BE6F986CF5679B680AB4C70E68B6'){throw 'GRUB changed'}
if((Test-Path 'D:\efi\boot\bootx64.efi') -or -not(Test-Path 'D:\efi\boot\bootx64.off')){throw 'Unexpected loader state'}
if((Get-FileHash 'D:\efi\boot\bootx64.off').Hash -ne '840E9A73F25507362E2A06CC8234FB51BEF8BD536122275194270C4FA7552F6C'){throw 'Loader changed'}
Rename-Item -LiteralPath 'D:\efi\boot\bootx64.off' -NewName 'bootx64.efi'
'linux_loader_enabled'
'normal_restart_request='+(Get-Date).ToString('s')
shutdown.exe /r /t 5 /d p:0:0 /c "BC250 E30 startup thermal and firmware read reference"
if($LASTEXITCODE -ne 0){throw 'Restart request failed'}
