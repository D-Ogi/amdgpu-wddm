$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted035'
if(Test-Path "$d\colour\process.json") {
 $identity=Get-Content "$d\colour\process.json" -Raw | ConvertFrom-Json
 $process=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
 if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Original colour process still active'}
}
# The independent collector must be terminal before receipts or task cleanup.
if(Test-Path "$d\collector-start.json"){
 $identity=Get-Content "$d\collector-start.json" -Raw | ConvertFrom-Json
 $process=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
 if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Original startup collector still active'}
 if(!(Test-Path "$d\collector-done.json")){throw 'Startup collector has no terminal receipt'}
}
if((Get-ScheduledTask -TaskName BC250-G0-DwmRun035 -ErrorAction SilentlyContinue).State -eq 'Running'){throw 'Runner still live'}
if(!(Test-Path "$d\done.json") -or !(Test-Path "$d\restored.json")){throw 'Missing terminal receipts'}
if(Test-Path "$d\interop-pending"){throw 'Interop rollback still pending'}
$zip="$d\receipts.zip"
if(Test-Path $zip){throw 'Archive already exists'}
$files=@(Get-ChildItem -LiteralPath $d -File | Where-Object {$_.Extension -in '.json','.log','.txt','.err','.png','.bmp'})
$paths=@($files.FullName)
if(Test-Path "$d\colour"){$paths+=Join-Path $d 'colour'}
Compress-Archive -LiteralPath $paths -DestinationPath $zip
Get-Item "$d\receipts.zip","$d\gpu.etl" | Select-Object Name,Length | ConvertTo-Json
