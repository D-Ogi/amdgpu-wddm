$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }
$out='C:\BC250\m9\pagefile32'
if(Test-Path $out){throw 'Pagefile preparation output already exists'}
$cs=Get-CimInstance Win32_ComputerSystem
$volume=Get-Volume -DriveLetter C
$disk=Get-Partition -DriveLetter C | Get-Disk
if([string]$disk.BusType -ne 'NVMe'){throw 'C is not on the expected NVMe bus'}
if($volume.SizeRemaining -lt 80GB){throw 'Insufficient disk reserve for fixed pagefile'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070085' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded133 full table required'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
if((Get-FileHash -LiteralPath $image).Hash -ne '37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87'){throw 'Installed image differs'}
$settings=@(Get-CimInstance Win32_PageFileSetting)
if(@($settings | Where-Object {$_.Name -ne 'C:\pagefile.sys'}).Count){throw 'Additional pagefile settings need review'}
New-Item -ItemType Directory $out | Out-Null
[pscustomobject]@{Time=(Get-Date).ToString('s');Automatic=$cs.AutomaticManagedPagefile;Settings=@($settings | Select-Object Name,InitialSize,MaximumSize);Usage=@(Get-CimInstance Win32_PageFileUsage | Select-Object Name,AllocatedBaseSize,CurrentUsage,PeakUsage);FreeDisk=$volume.SizeRemaining;Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')} | ConvertTo-Json -Depth 4 | Set-Content "$out\before.json" -Encoding UTF8
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before-driver.log"
$cs | Set-CimInstance -Property @{AutomaticManagedPagefile=$false}
$setting=Get-CimInstance Win32_PageFileSetting | Where-Object Name -eq 'C:\pagefile.sys'
if($setting){$setting | Set-CimInstance -Property @{InitialSize=[uint32]32768;MaximumSize=[uint32]32768}}
else{New-CimInstance -ClassName Win32_PageFileSetting -Property @{Name='C:\pagefile.sys';InitialSize=[uint32]32768;MaximumSize=[uint32]32768} | Out-Null}
$after=Get-CimInstance Win32_PageFileSetting | Where-Object Name -eq 'C:\pagefile.sys'
$after | Select-Object Name,InitialSize,MaximumSize | Format-List
if($after.InitialSize -ne 32768 -or $after.MaximumSize -ne 32768 -or (Get-CimInstance Win32_ComputerSystem).AutomaticManagedPagefile){throw 'Pagefile settings verification failed'}
Get-CimInstance Win32_PageFileUsage | Select-Object Name,AllocatedBaseSize,CurrentUsage,PeakUsage | Format-List
'pagefile32_configuration_verified_activation_pending_reboot'
& C:\BC250\m8\bc250kmd_cli.exe confirm
if($LASTEXITCODE -ne 0){throw 'Guard confirmation failed'}
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
if((Get-ItemProperty $reg).UnconfirmedStarts -ne 0){throw 'Guard count not cleared'}
New-ItemProperty $reg -Name EnableFullWddm -Value 1 -PropertyType DWord -Force | Out-Null
'Reboot requested for pagefile activation and133 OS startup test at '+(Get-Date).ToString('s')
shutdown.exe /r /t 10 /d p:0:0 /c 'BC250 lab: activate fixed32GiB pagefile and verify KMD133 startup'
if($LASTEXITCODE -ne 0){throw 'Restart request failed'}
