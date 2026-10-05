$ErrorActionPreference='Stop'
. "$PSScriptRoot\handshake-observation.ps1"
$good="HANDSHAKE n=1 hr=0x00263005 fmt=87 handle=0x00000001 update=2 ms=12`nSUMMARY ordinal100=1 ordinal101=1 handshakes=1 handshake_hr=0x00263005 handle_kind=global destination_ready=0 first_success=none exit=0`n"
$good="SHARED kind=global handle=0x00000001 luid=00000000:00000001 ours=1`n"+$good
if(!(Get-HandshakeObservation -Transcript $good -ExitCode 0).gpu_surface_offered){throw 'GDI answer lost'}
foreach($item in @(@('00263008','gdi_blit_required'),@('00000000','dedicated_dx_surface'),@('80263005','adapter_not_found'),@('80263003','no_redirection_surface'))){
 $r=Get-HandshakeObservation -Transcript $good.Replace('00263005',$item[0]) -ExitCode 0
 if($r.outcome -ne $item[1] -or $r.gpu_surface_offered -or $r.present_tested){throw 'Wrong result interpretation'}
}
$absent='SUMMARY ordinal100=0 ordinal101=1 handshakes=0 handshake_hr=0x00000000 handle_kind=unknown destination_ready=0 first_success=none exit=3'
if((Get-HandshakeObservation -Transcript $absent -ExitCode 3).outcome -ne 'ordinal_absent'){throw 'Absent ordinal control'}
$cases=@($good+"PRESENT variant=A`n",$good+"SOURCE halloc=1`n",$good+"OPENED numalloc=1`n",$good+"HANDSHAKE n=2 timeout_ms=2000`n",$good.Replace('handshakes=1','handshakes=2'),$good.Replace('n=1','n=2'),$good.Replace('handshake_hr=0x00263005','handshake_hr=0x00000000'))
foreach($text in $cases){$rejected=$false;try{Get-HandshakeObservation -Transcript $text -ExitCode 0|Out-Null}catch{$rejected=$true};if(!$rejected){throw 'Malformed/out-of-scope observation accepted'}}
$rejected=$false;try{Get-HandshakeObservation -Transcript $good -ExitCode 4|Out-Null}catch{$rejected=$true};if(!$rejected){throw 'Failed worker accepted'}
'PASS DWM response classification, absent ordinal, no Present claim, timeout/scope/sequence/exit rejection'
