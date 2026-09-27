$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted026'
if((Get-ScheduledTask -TaskName BC250-G0-DwmRun026 -ErrorAction SilentlyContinue).State -eq 'Running'){throw 'Runner still live'}
if(!(Test-Path "$d\done.json") -or !(Test-Path "$d\restored.json")){throw 'Missing terminal receipts'}
if(Test-Path "$d\interop-pending"){throw 'Interop rollback still pending'}
$zip="$d\receipts.zip"
if(Test-Path $zip){throw 'Archive already exists'}
$files=@(Get-ChildItem -LiteralPath $d -File | Where-Object {$_.Extension -in '.json','.log','.png','.bmp'})
Compress-Archive -LiteralPath $files.FullName -DestinationPath $zip
Get-Item "$d\receipts.zip","$d\gpu.etl" | Select-Object Name,Length | ConvertTo-Json
