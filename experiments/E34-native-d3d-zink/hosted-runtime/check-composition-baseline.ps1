param([Parameter(Mandatory)][string]$Image)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$bitmap=New-Object System.Drawing.Bitmap([IO.Path]::GetFullPath($Image))
try {
 if($bitmap.Width -lt 290 -or $bitmap.Height -lt 230){throw 'Composition image too small'}
 $regions=@(@{name='red';left=110;top=180;right=145;bottom=220;r=255;g=0;b=0},
            @{name='overlap';left=180;top=170;right=290;bottom=230;r=127;g=0;b=128})
 $mismatch=0;$pixels=0;$checks=@()
 foreach($region in $regions){
  $bad=0;$count=0
  for($y=$region.top;$y -lt $region.bottom;$y++){
   for($x=$region.left;$x -lt $region.right;$x++){
    $c=$bitmap.GetPixel($x,$y);$count++
    if($c.R -ne $region.r -or $c.G -ne $region.g -or $c.B -ne $region.b){$bad++}
   }
  }
  $checks+=@{region=$region.name;pixels=$count;mismatches=$bad}
  $mismatch+=$bad;$pixels+=$count
 }
 [pscustomobject]@{pass=($mismatch -eq 0 -and $pixels -eq 8000);pixels=$pixels;mismatches=$mismatch;regions=$checks}
} finally {$bitmap.Dispose()}
