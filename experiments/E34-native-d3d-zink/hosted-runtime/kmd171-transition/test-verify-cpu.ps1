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
Write-Output "PASS: CPU witness and $($cases.Count) false-closure controls"
