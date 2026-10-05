$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted028'
$cfg=Get-Content "$d\manifest.json" -Raw | ConvertFrom-Json
foreach($p in $cfg.PSObject.Properties){
 if((Get-FileHash -LiteralPath "$d\$($p.Name)").Hash -ne $p.Value){throw 'Staged hash mismatch'}
}
foreach($p in Get-ChildItem -LiteralPath $d -Filter '*.ps1'){
 $tokens=$null;$errors=$null
 [System.Management.Automation.Language.Parser]::ParseFile($p.FullName,[ref]$tokens,[ref]$errors)|Out-Null
 if($errors.Count){throw ('PS5 parse failed: '+$p.Name)}
}
if(Test-Path "$d\started"){throw 'Trial already started'}
if(Test-Path "$d\interop-pending"){throw 'Unexpected pending mutation'}
if((Get-FileHash 'C:\BC250\m12\candidate07163\rollback162\bc250kmd.sys').Hash -ne '58189FA6DC0C6F28FD2D6C153E314BF35C6344365F02C7DBB16A0F3BD48F8F4D'){throw 'Exact rollback162 missing'}
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$before=(Get-ItemProperty $reg).EnableCddDwmInterop
if($before -ne 0){throw 'Interop not baseline'}
& "$d\interop.ps1" -Value 0
if((Get-ItemProperty $reg).EnableCddDwmInterop -ne $before){throw 'No-op restore changed gate'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
@{utc=[DateTime]::UtcNow.ToString('o');hashes_pass=$true;ps5_parse_pass=$true;restore_without_marker_noop=$true;rollback162_present=$true;started=$false} | ConvertTo-Json
