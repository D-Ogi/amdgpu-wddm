$ErrorActionPreference='Stop'
. "$PSScriptRoot\scene-gate.ps1"
function Fixture {
 [pscustomobject]@{result='measured';exit=0;mode='offscreen';width=64;height=64;scenes=@(
  [pscustomobject]@{name='draws';frames=3;warmup=0;api_failures=0;gpu_failures=0;gpu_disjoint=0;draws=8;checksum='1111111111111111'},
  [pscustomobject]@{name='fill';frames=3;warmup=0;api_failures=0;gpu_failures=0;gpu_disjoint=0;layers=2;checksum='2222222222222222'},
  [pscustomobject]@{name='shaders';shaders=4;checksum='3333333333333333'})}
}
$cpu=Fixture;Assert-M14Scenes (Fixture) $cpu
foreach($kind in @('missing','duplicate','api','query','disjoint','count','checksum','workload')){
 $r=Fixture
 switch($kind){
  missing{$r.scenes=$r.scenes[0..1]}
  duplicate{$r.scenes[1].name='draws'}
  api{$r.scenes[0].api_failures=1}
  query{$r.scenes[0].gpu_failures=1}
  disjoint{$r.scenes[0].gpu_disjoint=1}
  count{$r.scenes[0].frames=2}
  checksum{$r.scenes[2].checksum='4444444444444444'}
  workload{$r.scenes[1].layers=1}
 }
 $rejected=$false;try{Assert-M14Scenes $r $cpu}catch{$rejected=$true}
 if(!$rejected){throw "Accepted bad scene fixture: $kind"}
}
'PASS scene completeness, workloads, API/query failures and CPU/GPU checksum gate'
