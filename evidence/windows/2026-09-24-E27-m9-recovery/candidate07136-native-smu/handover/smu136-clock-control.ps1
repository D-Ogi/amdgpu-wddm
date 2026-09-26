$ErrorActionPreference='Stop'
$cli='C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
function SmnTemperature {
 $text=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 if($LASTEXITCODE -ne 0 -or $text -notmatch 'raw ([0-9A-Fa-f]{8})'){throw 'SMN read failed'}
 $v=[Convert]::ToUInt32($Matches[1],16)
 $mc=[int](($v -shr 21) -band 0x7FF)*125
 if(($v -band 0x80000) -ne 0){$mc-=49000}
 if($mc -ge 85000){throw 'Temperature limit'}
 return $mc
}
SmnTemperature
& $cli clock set 1000 820
if($LASTEXITCODE -ne 0){throw 'Native SET failed'}
for($i=0;$i -lt 8;$i++){
 $before=SmnTemperature
 $read=& $cli clock read | Out-String
 if($LASTEXITCODE -ne 0 -or $read -notmatch 'MHz=1000 VID=116 temperature_mc=(-?[0-9]+) ready=1'){throw 'Typed paired read mismatch'}
 $native=[int]$Matches[1]
 $after=SmnTemperature
 $ok=$native -ge [Math]::Min($before,$after) -and $native -le [Math]::Max($before,$after)
 'sample='+$i+' smn_before_mc='+$before+' native_mc='+$native+' smn_after_mc='+$after+' bracket='+$ok
 if(-not $ok){throw 'Temperature sources disagree outside bracket'}
 Start-Sleep -Milliseconds 150
}
Get-Process bc250mon | ForEach-Object {
 'monitor_pid='+$_.Id+' start='+$_.StartTime.ToString('s')
 $_.Modules | Where-Object ModuleName -eq 'bc250control.dll' | ForEach-Object {'monitor_control='+$_.FileName+' sha256='+(Get-FileHash $_.FileName).Hash}
}
Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV','BC250 monitor overlay' | Select-Object TaskName,State | Format-Table
'paired_clock_control_complete='+(Get-Date).ToString('s')
