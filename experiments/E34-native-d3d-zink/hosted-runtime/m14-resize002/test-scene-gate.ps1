$ErrorActionPreference='Stop'
. "$PSScriptRoot\scene-gate.ps1"
$reference=Get-Content "$PSScriptRoot\resize-reference.json" -Raw|ConvertFrom-Json
function Fixture {
 $scene=$reference|ConvertTo-Json -Depth 10|ConvertFrom-Json
 [pscustomobject]@{result='measured';exit=0;mode='window';width=64;height=64;scene_revision=2;scenes=@($scene)}
}
Assert-M14Resize (Fixture) $reference
foreach($kind in @('revision','mode','exit','missing','resizes','presents','checks','steps','width','height','framecount','hashcount','pixel')){
 $r=Fixture
 switch($kind){
  revision{$r.scene_revision=1}
  mode{$r.mode='offscreen'}
  exit{$r.exit=1}
  missing{$r.scenes=@()}
  resizes{$r.scenes[0].resizes=2}
  presents{$r.scenes[0].presents=11}
  checks{$r.scenes[0].pixel_checks=11}
  steps{$r.scenes[0].steps=$r.scenes[0].steps[0..2]}
  width{$r.scenes[0].steps[1].width=64}
  height{$r.scenes[0].steps[1].height=32}
  framecount{$r.scenes[0].steps[1].presents=2}
  hashcount{$r.scenes[0].steps[1].checksums=@('0')}
  pixel{$r.scenes[0].steps[1].checksums[0]='0000000000000000'}
 }
 $rejected=$false
 try{Assert-M14Resize $r $reference}catch{$rejected=$true}
 if(!$rejected){throw "Accepted invalid resize result: $kind"}
}
'PASS resize oracle and 13 negative controls'
