function Assert-M14Scenes($Result,$Reference=$null) {
 $names=@('draws','fill','shaders')
 if($Result.result -ne 'measured' -or $Result.exit -ne 0 -or $Result.mode -ne 'offscreen' -or $Result.width -ne 64 -or $Result.height -ne 64){throw 'Unexpected scene result/settings'}
 if(@($Result.scenes).Count -ne 3){throw 'Incomplete scene set'}
 for($i=0;$i -lt 3;$i++){
  $scene=$Result.scenes[$i]
  if($scene.name -cne $names[$i] -or $scene.checksum -cnotmatch '^[0-9a-f]{16}$'){throw 'Unexpected scene identity/checksum'}
  if($i -lt 2){
   if($scene.frames -ne 3 -or $scene.warmup -ne 0 -or $scene.api_failures -ne 0 -or $scene.gpu_failures -ne 0 -or $scene.gpu_disjoint -ne 0){throw 'Scene API/query/count failure'}
  }
 }
 if($Result.scenes[0].draws -ne 8 -or $Result.scenes[1].layers -ne 2 -or $Result.scenes[2].shaders -ne 4){throw 'Wrong scene workload'}
 if($null -ne $Reference){
  Assert-M14Scenes $Reference
  for($i=0;$i -lt 3;$i++){
   if($Result.scenes[$i].checksum -cne $Reference.scenes[$i].checksum){throw "Image checksum mismatch: $($names[$i]) CPU=$($Reference.scenes[$i].checksum) GPU=$($Result.scenes[$i].checksum)"}
  }
 }
}
