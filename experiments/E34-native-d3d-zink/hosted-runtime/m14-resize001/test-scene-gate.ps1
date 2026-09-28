$ErrorActionPreference='Stop'
. "$PSScriptRoot\scene-gate.ps1"
$reference=Get-Content "$PSScriptRoot\resize-reference.json" -Raw|ConvertFrom-Json
function Fixture { return @{result='measured';exit=0;mode='window';width=64;height=64;scene_revision=2;scenes=@(($reference|ConvertTo-Json -Depth 8|ConvertFrom-Json))} }
Assert-M14Resize (Fixture) $reference
$mutations=@(
 {param($v)$v.scenes[0].steps[2].checksums[1]='0000000000000000'},
 {param($v)$v.scenes[0].steps=$v.scenes[0].steps[0..2]},
 {param($v)$v.scenes[0].presents=11},
 {param($v)$v.scenes[0].steps[1].width=64},
 {param($v)$v.exit=1},
 {param($v)$v.scene_revision=1}
)
foreach($change in $mutations){
 $v=Fixture;& $change $v;$rejected=$false
 try{Assert-M14Resize $v $reference}catch{$rejected=$true}
 if(!$rejected){throw 'Invalid resize fixture accepted'}
}
'PASS exact resize oracle, count, dimensions, status and revision controls'
