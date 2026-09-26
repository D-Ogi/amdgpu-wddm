$Name='concurrent';$Bytes='67108864';$Heap='vram'
$ErrorActionPreference='Stop'
$out='C:\BC250\m9\concurrent-residency07105'
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
if($version -ne '0.7.105.1'){throw 'Unexpected driver'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne '40D7E3790C8168A08B5BF21379EDC49D3B6288B477DBBEFFCED422E64C187008'){throw 'Installed SYS mismatch'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070069' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded full driver mismatch'}
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
$children=@()
foreach($client in @('client-a','client-b')) {
 @"
@echo off
"$out\gpu-residency-probe.exe" $Bytes $Heap > "$out\$client.out" 2> "$out\$client.err"
echo %ERRORLEVEL% > "$out\$client.exit"
"@ | Set-Content "$out\$client.cmd" -Encoding ASCII
 $p=Start-Process cmd.exe -ArgumentList "/c $out\$client.cmd" -WindowStyle Hidden -PassThru
 $null=$p.Handle
 $children+=@{Name=$client;Process=$p}
 "client_started=$client pid=$($p.Id) time=$((Get-Date).ToString('o'))"
}
$alive=0
foreach($c in $children){$c.Process.Refresh();if(-not $c.Process.HasExited){$alive++}}
'overlapping_live_launchers='+$alive
try {
 do {
  Start-Sleep -Seconds 2
  Check-Temperature
  $alive=0
  foreach($c in $children){$c.Process.Refresh();if(-not $c.Process.HasExited){$alive++}}
 } while($alive)
} catch {
 foreach($c in $children){$c.Process.Refresh();if(-not $c.Process.HasExited){& taskkill.exe /PID $c.Process.Id /T /F | Out-Null}}
 throw
}
foreach($c in $children){
 $c.Process.WaitForExit()
 $client=$c.Name
 $code=[int](Get-Content "$out\$client.exit")
 "client_finished=$client exit=$code"
 if($code -ne 0){throw 'Concurrent probe failed; inspect without repeating'}
 Get-Content "$out\$client.out" -Tail 2
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\concurrent-after.log"
if($LASTEXITCODE -ne 0){throw 'Final driver log unavailable'}
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'concurrent_run_complete'
