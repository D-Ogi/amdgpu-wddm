param([ValidatePattern('^[a-z0-9-]+$')][string]$Run)
$ErrorActionPreference='Stop'
$out="C:\BC250\m12\$Run";$tools='C:\BC250\m12\system-icd\tools'
if(Test-Path "$out\start.json"){throw 'Existing run'}
if(@(Get-ScheduledTask 'BC250-M12-*' | Where-Object State -in @('Running','Queued')).Count){throw 'M12 task still active'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$exe='C:\BC250\m10\cts-smoke-06\deqp-vk.exe'
if((Get-FileHash $exe).Hash -ne '35CBBC05F04C3B974B8D0638E102A854F78CB5E56B6E56FDA37D1BFEE452183F'){throw 'CTS binary mismatch'}
Copy-Item -LiteralPath $exe -Destination "$tools\deqp-vk.exe"
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
foreach($mode in @('clock','health')){
 $p=Start-Process $cli -ArgumentList $mode,'read' -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out\preflight-$mode.txt"
 $handle=$p.Handle
 if(-not $p.WaitForExit(4000)){Stop-Process -Id $p.Id -Force;throw "$mode timeout"}
 if($p.ExitCode -ne 0){throw "$mode exit"}
}
$clock=Get-Content "$out\preflight-clock.txt" -Raw
if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
if((Get-Content "$out\preflight-health.txt" -Raw) -notmatch 'flags=15 generation=\d+ epoch=\d+'){throw 'Health/generation gate'}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'ICD hash mismatch'}
@("$out\cts-worker.ps1","$out\cases.txt","$tools\deqp-vk.exe","$env:windir\System32\vulkan-1.dll",'C:\BC250\m10\wsi-final\vulkan_radeon.dll') | ForEach-Object {Get-FileHash $_ | Select-Object Path,Hash} | ConvertTo-Json | Set-Content "$out\inputs.json"
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $out\cts-worker.ps1 -Out $out"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName "BC250-M12-$Run" -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2)) | Out-Null
Start-ScheduledTask "BC250-M12-$Run"
'CTS_STARTED'
