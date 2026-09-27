param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
if(Test-Path $OutputDirectory){throw 'Use a fresh test directory'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
Add-Type -AssemblyName System.Drawing
$bitmap=New-Object System.Drawing.Bitmap(1000,600)
$graphics=[Drawing.Graphics]::FromImage($bitmap)
try {
 $graphics.Clear([Drawing.Color]::Black)
 $graphics.FillRectangle([Drawing.Brushes]::Red,110,180,35,40)
 $brush=New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(127,0,128))
 try {$graphics.FillRectangle($brush,180,170,110,60)} finally {$brush.Dispose()}
 $graphics.FillRectangle([Drawing.Brushes]::Cyan,608,151,184,101)
 $state=@{baseline_ready=$true;baseline_result=0;animating=$false;frames=0;paint_red=1;paint_blue=1;paint_moving=1;moving_client=@(608,151,792,252)}
 $witness=Join-Path $OutputDirectory 'heartbeat.json'
 $state | ConvertTo-Json | Set-Content $witness
 $good=Join-Path $OutputDirectory 'good.bmp';$bitmap.Save($good)
 $check=& "$PSScriptRoot\check-stable-composition.ps1" -Image $good -Heartbeat $witness
 if(!$check.pass -or $check.pixels -ne 26584){throw 'Positive image control failed'}
 $final=$state.Clone();$final.animating=$true;$final.frames=20;$final.frozen=$true;$final.freeze_result=0
 $final | ConvertTo-Json | Set-Content $witness
 if(!(& "$PSScriptRoot\check-stable-composition.ps1" -Image $good -Heartbeat $witness -Frozen).pass){throw 'Frozen positive control failed'}
 $final.frozen=$false;$final | ConvertTo-Json | Set-Content $witness
 $rejected=$false
 try {& "$PSScriptRoot\check-stable-composition.ps1" -Image $good -Heartbeat $witness -Frozen | Out-Null} catch {$rejected=$true}
 if(!$rejected){throw 'Live animation accepted as frozen'}
 $state | ConvertTo-Json | Set-Content $witness
 # One bottom corner pixel must fail, with no rounded-corner mask/tolerance.
 $bitmap.SetPixel(791,251,[Drawing.Color]::Black)
 $bad=Join-Path $OutputDirectory 'corner.bmp';$bitmap.Save($bad)
 $check=& "$PSScriptRoot\check-stable-composition.ps1" -Image $bad -Heartbeat $witness
 if($check.pass -or $check.mismatches -ne 1){throw 'Corner corruption was not detected'}
 # A torn/missing vertical strip must fail even though the old static ROIs pass.
 $graphics.FillRectangle([Drawing.Brushes]::Black,700,151,1,101)
 $bad=Join-Path $OutputDirectory 'strip.bmp';$bitmap.Save($bad)
 $check=& "$PSScriptRoot\check-stable-composition.ps1" -Image $bad -Heartbeat $witness
 if($check.pass -or $check.mismatches -ne 102){throw 'Missing strip was not detected'}
 foreach($variant in 'animating','unflushed','empty','outside') {
  $invalid=$state.Clone()
  switch($variant){
   animating {$invalid.animating=$true;$invalid.frames=5}
   unflushed {$invalid.baseline_result=1}
   empty {$invalid.moving_client=@(608,151,608,252)}
   outside {$invalid.moving_client=@(608,151,1001,252)}
  }
  $invalid | ConvertTo-Json | Set-Content $witness
  $rejected=$false
  try {& "$PSScriptRoot\check-stable-composition.ps1" -Image $good -Heartbeat $witness | Out-Null} catch {$rejected=$true}
  if(!$rejected){throw "Invalid witness accepted: $variant"}
 }
 'PASS full-client positive control, single corner pixel, torn strip, animation/flush/geometry rejection'
} finally {$graphics.Dispose();$bitmap.Dispose()}
