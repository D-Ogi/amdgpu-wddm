$ErrorActionPreference='Stop'
$out='C:\BC250\m9\pagefile32'
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'boot='+$boot
if($boot -eq '2026-09-24T11:44:14'){throw 'Old boot still active'}
$usage=Get-CimInstance Win32_PageFileUsage | Where-Object Name -eq 'C:\pagefile.sys'
$usage | Select-Object Name,AllocatedBaseSize,CurrentUsage,PeakUsage,TempPageFile | Format-List
if($usage.AllocatedBaseSize -ne 32768 -or $usage.TempPageFile){throw 'Requested pagefile not active'}
Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory | Select-Object CommittedBytes,CommitLimit,AvailableBytes | Format-List
Get-Volume -DriveLetter C | Select-Object Size,SizeRemaining | Format-List
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne '37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87'){throw 'Wrong installed SYS'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070085' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Expected133 full table not active'}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Tee-Object "$out\post-boot-driver.log"
if($LASTEXITCODE -ne 0){throw 'Driver log read failed'}
Get-Process dwm | ForEach-Object {
 'dwm_pid='+$_.Id+' start='+$_.StartTime.ToString('s')+' responding='+$_.Responding
 $_.Modules | Where-Object ModuleName -eq 'bc250d3d.dll' | ForEach-Object {'dwm_module='+$_.FileName+' sha256='+(Get-FileHash $_.FileName).Hash}
}
& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1
if($LASTEXITCODE -ne 0){throw 'Temperature read failed'}
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Startup clock not at expected settings'}
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object EnableFullWddm,UnconfirmedStarts,EnablePresentBlit,EnableDcnWrite,EnableVidPnFlip | Format-List
'pagefile32_post_boot_verified_at='+(Get-Date).ToString('s')
