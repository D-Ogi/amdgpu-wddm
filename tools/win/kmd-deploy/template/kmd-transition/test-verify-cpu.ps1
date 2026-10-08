$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$saved=@{umd_registration=@('cpu.dll');icd_registration=@('baseline.json');umd_sha256='CPU-HASH'}
function New-Observation {
 return @{umd_registration=@('cpu.dll');icd_registration=@('baseline.json');
  parameters=@{EnableGpuPresentBlit=0;EnableCddDwmInterop=0;UnconfirmedStarts=0};
  dwm=@(@{pid=42;start='2026-09-28T00:00:00Z';modules=@(@{name='bc250d3d.dll';sha256='CPU-HASH'})})}
}
Assert-KmdCpuBaseline $saved (New-Observation)
$cases=@(
 @{name='missing DWM';change={$args[0].dwm=@()}},
 @{name='no loaded UMD';change={$args[0].dwm[0].modules=@()}},
 @{name='GPU UMD';change={$args[0].dwm[0].modules[0].sha256='GPU-HASH'}},
 @{name='ICD still loaded';change={$args[0].dwm[0].modules+=@{name='vulkan_radeon.dll';sha256='ICD-HASH'}}},
 @{name='missing process identity';change={$args[0].dwm[0].start=$null}},
 @{name='different registration';change={$args[0].umd_registration=@('gpu.dll')}},
 @{name='extra registration';change={$args[0].icd_registration+=@('other.json')}},
 @{name='missing registration';change={$args[0].icd_registration=@()}},
 @{name='present gate';change={$args[0].parameters.EnableGpuPresentBlit=1}},
 @{name='interop gate';change={$args[0].parameters.EnableCddDwmInterop=1}},
 @{name='unconfirmed start';change={$args[0].parameters.UnconfirmedStarts=1}},
 @{name='missing gate';change={$args[0].parameters.Remove('EnableCddDwmInterop')}},
 @{name='second GPU DWM';change={$args[0].dwm+=@{pid=43;start='now';modules=@(@{name='bc250d3d.dll';sha256='GPU-HASH'})}}}
)
foreach($case in $cases){
 $observation=New-Observation
 & $case.change $observation
 $rejected=$false
 try{Assert-KmdCpuBaseline $saved $observation}catch{$rejected=$true}
 if(!$rejected){throw "False acceptance: $($case.name)"}
}
# The router desktop (GPU DWM ladder T3 and later): the capture names switches 1 and the CPU route's module set,
# router + CPU UMD. The same verifier must accept exactly that and nothing else.
$routerSaved=@{umd_registration=@('router.dll','router.dll');icd_registration=@('baseline.json');umd_sha256='CPU-HASH';
 desktop=([pscustomobject]@{switches=1;modules=@('CPU-HASH','ROUTER-HASH')})}
function New-RouterObservation {
 return @{umd_registration=@('router.dll','router.dll');icd_registration=@('baseline.json');
  parameters=@{EnableGpuPresentBlit=1;EnableCddDwmInterop=1;UnconfirmedStarts=0;InteropLastState=0x303};
  dwm=@(@{pid=42;start='2026-10-01T00:00:00Z';modules=@(@{name='bc250d3d_router.dll';sha256='ROUTER-HASH'},@{name='bc250d3d.dll';sha256='CPU-HASH'})})}
}
Assert-KmdCpuBaseline $routerSaved (New-RouterObservation)
$routerCases=@(
 @{name='GPU route modules';change={$args[0].dwm[0].modules=@(@{name='bc250d3d_router.dll';sha256='ROUTER-HASH'},@{name='bc250d3d_zink.dll';sha256='ZINK-HASH'},@{name='amdgpu_wddm_radv.dll';sha256='ICD-HASH'})}},
 @{name='GPU route plus CPU UMD';change={$args[0].dwm[0].modules+=@{name='bc250d3d_zink.dll';sha256='ZINK-HASH'}}},
 @{name='router missing';change={$args[0].dwm[0].modules=@(@{name='bc250d3d.dll';sha256='CPU-HASH'})}},
 @{name='router twice';change={$args[0].dwm[0].modules+=@{name='bc250d3d_router.dll';sha256='ROUTER-HASH'}}},
 @{name='CPU UMD missing';change={$args[0].dwm[0].modules=@(@{name='bc250d3d_router.dll';sha256='ROUTER-HASH'})}},
 @{name='switches written 0';change={$args[0].parameters.EnableGpuPresentBlit=0;$args[0].parameters.EnableCddDwmInterop=0}},
 @{name='one switch 0';change={$args[0].parameters.EnableCddDwmInterop=0}},
 @{name='switch absent';change={$args[0].parameters.Remove('EnableGpuPresentBlit')}},
 @{name='latch closed 0x000';change={$args[0].parameters.InteropLastState=0}},
 @{name='latch unclean 0x300';change={$args[0].parameters.InteropLastState=0x300}},
 @{name='latch absent';change={$args[0].parameters.Remove('InteropLastState')}},
 @{name='registration back to CPU';change={$args[0].umd_registration=@('cpu.dll','cpu.dll')}},
 @{name='unconfirmed start';change={$args[0].parameters.UnconfirmedStarts=1}}
)
foreach($case in $routerCases){
 $observation=New-RouterObservation
 & $case.change $observation
 $rejected=$false
 try{Assert-KmdCpuBaseline $routerSaved $observation}catch{$rejected=$true}
 if(!$rejected){throw "False acceptance (router desktop): $($case.name)"}
}
# A legacy capture (no desktop record) still refuses the router desktop: the old rule is unchanged.
$legacyRejected=$false
try{Assert-KmdCpuBaseline @{umd_registration=@('router.dll','router.dll');icd_registration=@('baseline.json');umd_sha256='CPU-HASH'} (New-RouterObservation)}catch{$legacyRejected=$true}
if(!$legacyRejected){throw 'False acceptance: router desktop under a legacy capture'}
# A malformed desktop record is refused, never read as the CPU desktop.
foreach($bad in @([pscustomobject]@{switches=2;modules=@('CPU-HASH')},[pscustomobject]@{switches=1;modules=@()})){
 $badRejected=$false
 try{Assert-KmdCpuBaseline (@{umd_registration=@('router.dll','router.dll');icd_registration=@('baseline.json');umd_sha256='CPU-HASH';desktop=$bad}) (New-RouterObservation)}catch{$badRejected=$true}
 if(!$badRejected){throw 'False acceptance: malformed desktop record'}
}
# The confirmed start a finished Verify admitted, read back from the acceptance receipts (BD-098 follow-up): a
# mode set after Verify advances the epoch and restarts the ready clock, so a later reader needs that epoch.
$records=@(
 [pscustomobject]@{scope='candidate-ready-only';health=[pscustomobject]@{flags=7;generation=43455941387;epoch=3;ready_ms=61000;completed=9;age_ms=100}},
 [pscustomobject]@{scope='candidate-confirmed';health=[pscustomobject]@{flags=15;generation=43455941387;epoch=5;ready_ms=60442;completed=76;age_ms=1241}},
 [pscustomobject]@{scope='restored-confirmed';health=[pscustomobject]@{flags=15;generation=99999999999;epoch=2;ready_ms=60001;completed=12;age_ms=300}}
)
if((Get-KmdAcceptedConfirmedEpoch -Records $records -Generation 43455941387) -ne 5){throw 'Accepted epoch of the live generation not found'}
if((Get-KmdAcceptedConfirmedEpoch -Records $records -Generation 99999999999) -ne 2){throw 'Accepted epoch of the other generation not found'}
# A generation no record names leaves the caller without a supersession, it does not invent one.
if((Get-KmdAcceptedConfirmedEpoch -Records $records -Generation 7) -ne 0){throw 'Accepted epoch invented for an unknown generation'}
if((Get-KmdAcceptedConfirmedEpoch -Records @() -Generation 7) -ne 0){throw 'Accepted epoch invented without records'}
$epochCases=@(
 @{name='no live generation';run={Get-KmdAcceptedConfirmedEpoch -Records $records -Generation 0}},
 @{name='record without a witness';run={Get-KmdAcceptedConfirmedEpoch -Records @([pscustomobject]@{scope='candidate-confirmed';health=$null}) -Generation 5}},
 @{name='confirmed record below the ready age';run={Get-KmdAcceptedConfirmedEpoch -Records @([pscustomobject]@{scope='candidate-confirmed';health=[pscustomobject]@{flags=15;generation=5;epoch=4;ready_ms=59999;completed=9;age_ms=100}}) -Generation 5}},
 @{name='confirmed record with unconfirmed flags';run={Get-KmdAcceptedConfirmedEpoch -Records @([pscustomobject]@{scope='candidate-confirmed';health=[pscustomobject]@{flags=7;generation=5;epoch=4;ready_ms=61000;completed=9;age_ms=100}}) -Generation 5}}
)
foreach($case in $epochCases){
 $rejected=$false
 try{& $case.run|Out-Null}catch{$rejected=$true}
 if(!$rejected){throw "False acceptance (accepted epoch): $($case.name)"}
}
Write-Output "PASS: CPU witness and $($cases.Count) false-closure controls; router desktop witness and $($routerCases.Count + 3) controls; accepted confirmed epoch and $($epochCases.Count) controls"
