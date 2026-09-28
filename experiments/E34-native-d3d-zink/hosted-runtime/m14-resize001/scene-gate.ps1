function Assert-M14Resize($Result,$Reference) {
 if($Result.result -ne 'measured' -or $Result.exit -ne 0 -or $Result.mode -ne 'window' -or $Result.width -ne 64 -or $Result.height -ne 64 -or $Result.scene_revision -ne 2){throw 'Resize result/settings failed'}
 if(@($Result.scenes).Count -ne 1){throw 'Resize scene missing or duplicated'}
 $s=$Result.scenes[0]
 if($s.name -cne 'resize' -or $s.resizes -ne 3 -or $s.presents -ne 12 -or $s.pixel_checks -ne 12 -or @($s.steps).Count -ne 4){throw 'Incomplete resize workload'}
 for($i=0;$i -lt 4;$i++){
  $a=$s.steps[$i];$b=$Reference.steps[$i]
  if($a.width -ne $b.width -or $a.height -ne $b.height -or $a.presents -ne 3 -or @($a.checksums).Count -ne 3){throw 'Wrong resize step'}
  for($f=0;$f -lt 3;$f++){if($a.checksums[$f] -cne $b.checksums[$f]){throw 'Resize pixel mismatch'}}
 }
}
