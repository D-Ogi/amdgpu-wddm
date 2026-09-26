$ErrorActionPreference='Stop'
$out='C:\BC250\m9\resume146-s4'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) {throw 'Owner STOP requested'}
if(Test-Path "$out\probe.out"){throw 'Output already exists'}
$exe=Join-Path $out 'gpu-residency-probe.exe'
if((Get-FileHash $exe).Hash -ne 'E9B690115E841AB7107D8D16219D320AD70D71595D7F9A9ED1F704150EE56D3A'){throw 'Probe hash mismatch'}
@"
@echo off
"$exe" 67108864 vram --resume-gate $out\ready.txt $out\release.txt 900000 > $out\probe.out 2> $out\probe.err
echo %ERRORLEVEL% > $out\probe.exit
"@ | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
$action=New-ScheduledTaskAction -Execute conhost.exe -Argument "--headless cmd.exe /c $out\run.cmd"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName BC250-M9-Resume146Probe -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Resume146Probe
$deadline=(Get-Date).AddSeconds(60)
do {
 Start-Sleep -Seconds 1
 if(Test-Path "$out\probe.exit"){throw 'Probe exited before gate'}
 if((Get-Date) -ge $deadline){Stop-ScheduledTask BC250-M9-Resume146Probe;throw 'Ready deadline'}
}while(-not (Test-Path "$out\ready.txt"))
$ready=Get-Content "$out\ready.txt" -Raw
if($ready -notmatch 'first_full_readback=PASS'){throw 'Incomplete ready file'}
$ready
Get-Content "$out\probe.out" -Tail 8
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
if($LASTEXITCODE -ne 0){throw 'No health'}
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe log summary | Out-File "$out\before-driver.log"
Get-Process gpu-residency-probe,dwm,bc250mon | Select-Object ProcessName,Id,StartTime | ConvertTo-Json | Set-Content "$out\processes-before.json"
'probe_waiting_no_transition'
