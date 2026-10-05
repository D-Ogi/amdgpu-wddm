param([string]$Name='resident6g',[string]$Bytes='6442450944',[string]$Heap='vram')
$ErrorActionPreference='Stop'
$out='C:\BC250\m9\gpu-residency-v6-results'
New-Item -ItemType Directory -Force $out | Out-Null
if(Test-Path "$out\$Name.out"){throw 'Output already exists'}
Copy-Item 'C:\BC250\m9\gpu-residency-v6\gpu-residency-probe.exe' "$out\gpu-residency-probe.exe"
$hash=(Get-FileHash "$out\gpu-residency-probe.exe").Hash
'probe_hash='+$hash
if($hash -ne 'A8F942D3C9ACC41E4B29A3F4EA3478466F95E2617568BE7FDBFBC4A04F31C530'){throw 'Probe mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like '[PCI instance redacted]
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'No healthy adapter'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$version
if($version -ne '0.7.133.1'){throw 'Unexpected driver'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne '37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87'){throw 'Installed SYS mismatch'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070085' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded full driver mismatch'}
Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory | Select-Object CommittedBytes,CommitLimit,AvailableBytes | Format-List
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
function Check-Temperature {
 if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim() | Write-Host
 if($LASTEXITCODE -ne 0 -or $temp -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'No temperature'}
 if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
}
Check-Temperature
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\$Name-before.log"
if($LASTEXITCODE -ne 0){throw 'Driver log unavailable'}
@"
@echo off
"$out\gpu-residency-probe.exe" $Bytes $Heap --resident-only > "$out\$Name.out" 2> "$out\$Name.err"
echo %ERRORLEVEL% > "$out\$Name.exit"
"@ | Set-Content "$out\$Name.cmd" -Encoding ASCII
$p=Start-Process cmd.exe -ArgumentList "/c $out\$Name.cmd" -WindowStyle Hidden -PassThru
$null=$p.Handle
'launcher_pid='+$p.Id
try {
 do { Start-Sleep -Seconds 2; Check-Temperature; $p.Refresh() } while(-not $p.HasExited)
 $p.WaitForExit()
 $code=[int](Get-Content "$out\$Name.exit")
} catch {
 $p.Refresh()
 if(-not $p.HasExited){ & taskkill.exe /PID $p.Id /T /F | Out-Null }
 throw
}
Get-Content "$out\$Name.out" -Tail 12
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\$Name-after.log"
if($LASTEXITCODE -ne 0){throw 'Final driver log unavailable'}
'gpu_probe_exit='+$code
Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory | Select-Object CommittedBytes,CommitLimit,AvailableBytes | Format-List
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
if($code -ne 0){throw 'Probe failed; inspect output without repeating'}
