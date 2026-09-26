$ErrorActionPreference='Stop'
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$fw=@{
'cyan_skillfish2_sdma.bin'='15D0D3626DA7F2513F03B13BB7E02EEEECCAB276276AE27D0B6067B5F25E9E95'
'cyan_skillfish2_sdma1.bin'='BD1C0B0F6A6A4F17EDE034F2C844B6553FC444E08915C7BB500C09FDE59D6255'
'cyan_skillfish2_ce.bin'='5946EFBF7E46CCFE7E9F965DA56C35B1187469D1C1A49C8A9EFF433EB3AB4723'
'cyan_skillfish2_pfp.bin'='50E56DC1913571FF589425D2B059BDDFC5E8A2B4BC22D3EB78CAC75D1E0479A1'
'cyan_skillfish2_me.bin'='D4ED4D968DE0D7720F7C8215D42FDAE727BF99CDF43CEE6F8F864DC2584B17E2'
'cyan_skillfish2_mec.bin'='B1C1F843A8FAAA2DE537D6BA05052A3776B5A1B73F8A3C3F5ED9D568489776A4'
'cyan_skillfish2_mec2.bin'='B1C1F843A8FAAA2DE537D6BA05052A3776B5A1B73F8A3C3F5ED9D568489776A4'
'cyan_skillfish2_rlc.bin'='20ACEFDB6128A36275F4382425A109A7C9927F6053EAB685BB51164FF1D18CFB'
}
foreach($name in $fw.Keys) {
 $file=Get-Item -LiteralPath ("C:\BC250\firmware\"+$name)
 $hash=(Get-FileHash $file.FullName).Hash
 "firmware=$name bytes=$($file.Length) sha256=$hash"
 if($hash -ne $fw[$name]) { throw 'Firmware does not match local upstream copy' }
}
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK') { throw 'Expected healthy target adapter' }
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
"driver_version=$version"
if($version -ne '0.7.96.1') { throw 'Unexpected driver' }
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& 'C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe' clock-check 1000 820
if($LASTEXITCODE -ne 0) { throw 'Clock verification failed' }
$raw=& 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
$raw
if($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'Temperature unavailable' }
if([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
$enabled=@('EnableMmio','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','KeepLog')
foreach($n in $enabled) { New-ItemProperty $reg -Name $n -Value 1 -PropertyType DWord -Force | Out-Null }
foreach($n in @('EnableMmioWrite','EnableDcnWrite','EnableVidPnFlip','EnablePresentBlit')) { New-ItemProperty $reg -Name $n -Value 0 -PropertyType DWord -Force | Out-Null }
New-ItemProperty $reg -Name EnableFullWddm -Value 1 -PropertyType DWord -Force | Out-Null
Get-ItemProperty $reg | Select-Object Enable*,UnconfirmedStarts,KeepLog | Format-List
'disable_begin='+(Get-Date).ToString('s')
& pnputil.exe /disable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0) { throw 'PnP disable failed' }
Start-Sleep -Seconds 4
'enable_begin='+(Get-Date).ToString('s')
& pnputil.exe /enable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0) { throw 'PnP enable failed' }
Start-Sleep -Seconds 8
Get-PnpDevice -InstanceId $gpu[0].InstanceId | Select-Object Status,Problem | Format-List
Get-ItemProperty $reg | Select-Object EnableFullWddm,LastStage,StageHistory,UnconfirmedStarts | Format-List
& 'C:\BC250\m8\bc250kmd_cli.exe' info
& 'C:\BC250\m8\bc250kmd_cli.exe' log summary
& 'C:\BC250\m8\bc250kmd_cli.exe' log
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'trial_finished'
