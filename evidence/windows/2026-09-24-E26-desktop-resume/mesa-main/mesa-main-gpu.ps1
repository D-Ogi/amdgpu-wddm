param([string]$Name='mesa-main-64k',[string]$Bytes='65536',[string]$Heap='vram')
$ErrorActionPreference='Stop'
$out='C:\BC250\m9\gpu-residency07127-control'
New-Item -ItemType Directory -Force $out | Out-Null
if(Test-Path "$out\$Name.out"){throw 'Output already exists'}
Copy-Item 'C:\BC250\m9\gpu-residency0798-v4\gpu-residency-probe.exe' "$out\gpu-residency-probe.exe"
$hash=(Get-FileHash "$out\gpu-residency-probe.exe").Hash
'probe_hash='+$hash
if($hash -ne 'E9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3'){throw 'Probe mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'No healthy adapter'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$version
if($version -ne '0.7.127.1'){throw 'Unexpected driver'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne '476858800F28A5C103E43FECCC8595C6042BA2479EE18FCFCC3D9FD313953E90'){throw 'Installed SYS mismatch'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x0007007F' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded full driver mismatch'}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
function Check-Temperature {
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
"$out\gpu-residency-probe.exe" $Bytes $Heap > "$out\$Name.out" 2> "$out\$Name.err"
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
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
if($code -ne 0){throw 'Probe failed; inspect output without repeating'}
