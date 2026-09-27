# Pure transcript classifier for --handshake-only --no-open.
# A measured answer is not a successful GPU presentation.
function Get-HandshakeObservation {
 param([Parameter(Mandatory)][string]$Transcript,[Parameter(Mandatory)][int]$ExitCode)
 if($Transcript -match '(?m)^(PRESENT |RESULT variant=|SOURCE |OPENED |UPDATE |SHARED )'){throw 'Probe exceeded handshake-only/no-open scope'}
 if($Transcript -match 'D3DKMT(?:CreateAllocation2|CreateContextVirtual|OpenResource|Present|Lock2)'){throw 'Unexpected allocation/context/open/Present call'}
 if($Transcript -match '(?im)timeout_ms=|WATCHDOG fired|WAIT_FAILED'){throw 'Unresolved or timed-out handshake'}
 $summary=[regex]::Matches($Transcript,'(?m)^SUMMARY ordinal100=([01]) ordinal101=([01]) handshakes=(\d+) handshake_hr=0x([0-9A-Fa-f]{8}) first_success=none\s*$')
 if($summary.Count -ne 1){throw 'Missing or ambiguous terminal summary'}
 $last=$summary[0]
 if($last.Groups[1].Value -eq '0'){
  if($ExitCode -ne 3 -or [int]$last.Groups[3].Value -ne 0){throw 'Inconsistent absent-ordinal result'}
  return @{outcome='ordinal_absent';gpu_surface_offered=$false;present_tested=$false;calls=0}
 }
 if($ExitCode -ne 0){throw "Probe failed: exit $ExitCode"}
 $calls=[regex]::Matches($Transcript,'(?m)^HANDSHAKE n=(\d+) hr=0x([0-9A-Fa-f]{8}) fmt=(\d+) handle=(\S+) update=(\d+) ms=(\d+)\s*$')
 if(!$calls.Count -or $calls.Count -ne [int]$last.Groups[3].Value){throw 'Incomplete handshake records'}
 $records=@();$n=0
 foreach($call in $calls){
  $n++
  if([int]$call.Groups[1].Value -ne $n){throw 'Handshake sequence gap'}
  $records+=@{sequence=$n;hr=$call.Groups[2].Value.ToUpperInvariant();format=[int]$call.Groups[3].Value;handle=$call.Groups[4].Value;update_id=$call.Groups[5].Value;milliseconds=[int]$call.Groups[6].Value}
 }
 $hr=$records[-1].hr
 if($hr -ne $last.Groups[4].Value.ToUpperInvariant()){throw 'Summary differs from completed handshake'}
 $outcome=switch($hr){
  '00263005' {'gdi_surface_offered'}
  '00263008' {'gdi_blit_required'}
  '00000000' {'dedicated_dx_surface'}
  '80263005' {'adapter_not_found'}
  '80263003' {'no_redirection_surface'}
  default {'other_dwm_answer'}
 }
 return @{outcome=$outcome;gpu_surface_offered=($hr -eq '00263005');present_tested=$false;calls=$calls.Count;records=$records}
}
