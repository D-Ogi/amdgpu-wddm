$ErrorActionPreference='Stop'
if(@(Get-Process cross-process-control,cross-process-duplicate-control -ErrorAction SilentlyContinue).Count){throw 'Control remains active'}
$removed=@()
foreach($n in @(123,124,125)){
 $name='BC250-G0-Present'+$n
 $t=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
 if($t -and $t.State -eq 'Running'){throw 'Worker still active'}
 if($t){Unregister-ScheduledTask -TaskName $name -Confirm:$false}
 $removed+=$name
}
$umd=(Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
$icd=(Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
if($umd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA' -or $icd -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'){throw 'Baseline restoration mismatch'}
$health=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read | Out-String
if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x0007009F flags=15'){throw 'Health gate failed'}
$clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate failed'}
$record=@{utc=[DateTime]::UtcNow.ToString('o');umd=$umd;icd=$icd;health=$health;clock=$clock;removed=$removed;dwm=@(Get-Process dwm | Select-Object -ExpandProperty Id);boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
$base='C:\BC250\m13\duplicate-control001';$hosted='C:\BC250\m13\hosted-runtime053';$root='C:\BC250\m13\runtime-probe001'
$record | ConvertTo-Json -Depth 4 | Set-Content "$base\closure.json"
$files=@(Get-ChildItem -LiteralPath $base -File | Where-Object {$_.Extension -in '.json','.txt','.log','.ps1'})
Compress-Archive -LiteralPath $files.FullName -DestinationPath "$base\receipts123.zip"
$paths=@()
foreach($n in @(124,125)){
 foreach($name in @("run$n.ps1","worker$n.ps1","launch$n.ps1","done$n.json","run$n.log","kmd-before$n.log","kmd-after$n.log")){if(Test-Path "$hosted\$name"){$paths+="$hosted\$name"}}
 foreach($name in @("stdout$n.txt","stderr$n.txt","mesa$n.log")){if(Test-Path "$root\$name"){$paths+="$root\$name"}}
}
Compress-Archive -LiteralPath $paths -DestinationPath "$base\receipts124-125.zip"
$record | ConvertTo-Json -Depth 4
