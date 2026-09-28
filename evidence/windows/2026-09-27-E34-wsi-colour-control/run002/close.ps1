$ErrorActionPreference='Stop'
$d='C:\BC250\m13\wsi-colour002';$name='BC250 WSI colour002'
if(!(Test-Path "$d\done.json")){throw 'No terminal receipt'}
$done=Get-Content "$d\done.json" -Raw | ConvertFrom-Json
if($done.process_alive){throw 'Worker says process alive'}
$identity=Get-Content "$d\process.json" -Raw | ConvertFrom-Json
$p=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
if($p -and $p.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Original probe still alive'}
if((Get-ScheduledTask -TaskName $name).State -eq 'Running'){throw 'Task still running'}
Unregister-ScheduledTask -TaskName $name -Confirm:$false
if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){throw 'Task retained'}
$done | ConvertTo-Json
Compress-Archive -Path "$d\*.json","$d\*.txt","$d\*.csv","$d\*.log","$d\*.png","$d\*.bmp" -DestinationPath "$d\result.zip"
