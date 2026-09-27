param([Parameter(Mandatory)][string]$Image,[Parameter(Mandatory)][string]$Heartbeat)
$ErrorActionPreference='Stop'
$state=Get-Content -LiteralPath $Heartbeat -Raw | ConvertFrom-Json
if($state.baseline_ready -ne $true -or $state.baseline_result -ne 0 -or $state.animating -ne $false -or $state.frames -ne 0){throw 'Stable initial composition witness required'}
foreach($name in 'paint_red','paint_blue','paint_moving'){if($state.$name -le 0){throw "Missing paint witness: $name"}}
$bounds=@($state.moving_client)
if($bounds.Count -ne 4){throw 'Missing client geometry'}
foreach($value in $bounds){if($null -eq $value -or $value -is [string] -or [double]$value -ne [int]$value){throw 'Invalid client coordinate'}}
Add-Type -AssemblyName System.Drawing
$bitmap=New-Object System.Drawing.Bitmap([IO.Path]::GetFullPath($Image))
try {
 if($bitmap.Width -lt 290 -or $bitmap.Height -lt 230){throw 'Composition image too small'}
 $regions=@(@{name='red';left=110;top=180;right=145;bottom=220;r=255;g=0;b=0},
            @{name='overlap';left=180;top=170;right=290;bottom=230;r=127;g=0;b=128})
 if($bounds[0] -lt 400 -or $bounds[1] -lt 0 -or $bounds[2] -le $bounds[0] -or $bounds[3] -le $bounds[1] -or $bounds[2] -gt $bitmap.Width -or $bounds[3] -gt $bitmap.Height){throw 'Client geometry outside image or overlapping static controls'}
 $regions+=@{name='moving_full_client';left=$bounds[0];top=$bounds[1];right=$bounds[2];bottom=$bounds[3];r=0;g=255;b=255}
 $expectedPixels=8000+($bounds[2]-$bounds[0])*($bounds[3]-$bounds[1])
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
 [pscustomobject]@{pass=($mismatch -eq 0 -and $pixels -eq $expectedPixels);pixels=$pixels;mismatches=$mismatch;regions=$checks}
} finally {$bitmap.Dispose()}
