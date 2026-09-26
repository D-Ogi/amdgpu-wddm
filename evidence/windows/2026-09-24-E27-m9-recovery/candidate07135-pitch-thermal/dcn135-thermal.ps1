$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070087' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Driver mismatch'}
function Read-Smn {
 $v=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 if($LASTEXITCODE -ne 0 -or $v -notmatch 'raw ([0-9A-Fa-f]{8})'){throw 'SMN read failed'}
 return [Convert]::ToUInt32($Matches[1],16)
}
function Decode([uint32]$raw) {
 $mc=[int](($raw -shr 21) -band 0x7ff)*125
 if($raw -band 0x80000){$mc-=49000}
 return $mc
}
$exact=0
for($i=0;$i -lt 12;$i++){
 if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }
 $start=(Get-Date).ToUniversalTime().ToString('o')
 $a=Read-Smn
 $v=& C:\BC250\m8\bc250kmd_cli.exe read 0x59800 | Out-String
 if($LASTEXITCODE -ne 0 -or $v -notmatch 'read\s+0x[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})'){throw ('BAR read failed: '+$v)}
 $b=[Convert]::ToUInt32($Matches[1],16)
 $c=Read-Smn
 $ma=Decode $a; $mb=Decode $b; $mc=Decode $c
 if([Math]::Max($ma,[Math]::Max($mb,$mc)) -ge 85000){throw 'Temperature limit'}
 if($a -eq $b -and $b -eq $c){$exact++}
 [ordered]@{sample=$i;start_utc=$start;end_utc=(Get-Date).ToUniversalTime().ToString('o');register='mmTHM_TCON_CUR_TMP';smn_before=('{0:X8}' -f $a);bar=('{0:X8}' -f $b);smn_after=('{0:X8}' -f $c);before_mc=$ma;bar_mc=$mb;after_mc=$mc} | ConvertTo-Json -Compress
 Start-Sleep -Milliseconds 250
}
'exact_raw_brackets='+$exact+'/12'
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'comparison_complete'
