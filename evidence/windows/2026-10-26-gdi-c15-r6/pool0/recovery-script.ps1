param([ValidateSet('enable-driver','gdi-accel-on')][string]$Action='enable-driver')
$ErrorActionPreference='Stop'
$app='C:\Program Files\amdgpu-wddm\control\amdgpu_wddm_control.exe'
if((Get-FileHash -LiteralPath $app).Hash -ne '7DB25E9732652FC0E0D91A157E5CE41C1AD8388FE4AAC2D15989554B0F3E53F2'){throw 'Unexpected control image'}
# A deliberately disabled PnP device must be enabled, not restarted while disabled.
# Confirm this one start after the driver's 60-second healthy interval.
if($Action -eq 'enable-driver'){
 $dev=Get-PnpDevice -Class Display -PresentOnly | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'} | Select-Object -First 1
 if($dev.Problem -eq 'CM_PROB_DISABLED'){
  $pn=Start-Process pnputil.exe -ArgumentList @('/enable-device', ('"'+$dev.InstanceId+'"')) -PassThru -WindowStyle Hidden
  if(-not $pn.WaitForExit(30000)){throw 'PnP enable exceeded bound; leave PnP in progress, no further mutation'}
  if($pn.ExitCode -ne 0){throw "PnP enable failed $($pn.ExitCode)"}
  Start-Sleep -Seconds 65
  $dev=Get-PnpDevice -InstanceId $dev.InstanceId
  if($dev.Status -ne 'OK'){throw 'Enabled device not healthy'}
  $Action='confirm-start'
 }
}
$out='C:\BC250\tmp\c15-recovery-'+$Action+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')+'.txt'
'utc '+[DateTime]::UtcNow.ToString('o')
$p=Start-Process -FilePath $app -ArgumentList @('--action',$Action,'--out',$out) -WindowStyle Hidden -PassThru
$null=$p.Handle
if(-not $p.WaitForExit(150000)){$p.Kill();throw 'Recovery action exceeded 150 seconds'}
if(Test-Path -LiteralPath $out){Get-Content -LiteralPath $out}
'action exit '+$p.ExitCode
exit $p.ExitCode
