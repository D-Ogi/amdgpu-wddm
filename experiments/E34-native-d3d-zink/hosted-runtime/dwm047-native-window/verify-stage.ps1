$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted047'
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
if((Get-FileHash 'C:\BC250\m12\candidate07166\rollback165\bc250kmd.sys').Hash -ne '548E9D8D7A699005D37876EF0097599D52E2C88AD67EC06EBD6723B319A9155C'){throw 'Exact rollback165 missing'}
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$before=(Get-ItemProperty $reg).EnableCddDwmInterop
if($before -ne 0){throw 'Interop not baseline'}
& "$d\interop.ps1" -Value 0
if((Get-ItemProperty $reg).EnableCddDwmInterop -ne $before){throw 'No-op restore changed gate'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
@{utc=[DateTime]::UtcNow.ToString('o');hashes_pass=$true;ps5_parse_pass=$true;restore_without_marker_noop=$true;rollback165_present=$true;started=$false} | ConvertTo-Json
