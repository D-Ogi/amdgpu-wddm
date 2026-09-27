$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted033'
$out='C:\BC250\m13\wsi-preflight001\checks'
New-Item -ItemType Directory -Path $out -ErrorAction Stop | Out-Null
$cfg=Get-Content "$d\manifest.json" -Raw | ConvertFrom-Json
$exe="$d\wsi-colour-control.exe"
$icd="$d\wsi-kmt.dll"
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if(Test-Path "$out\start.json"){throw 'Existing run'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if((Get-FileHash $exe).Hash -ne '228EBEC0A898EDB2454304F59BB38DD2E56A0BF519E9C5D06535446122EC07C3'){throw 'Probe hash'}
if((Get-FileHash $icd).Hash -ne '0CD4A98DD80AE1248CFA1E6D4C1EB650DCF217FA9A840C002EDC4BB7EB0ADB47'){throw 'ICD hash'}
function Snapshot {
 $health=& $cli health read | Out-String
 if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A4 flags=15'){throw 'KMD164 health gate'}
 $clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
 if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 $kmd=(Get-FileHash $image).Hash
 if($kmd -ne '9B9B99D3F3FA2A32816E71C8754A6BB6349A427C33CCCD1E9B09C08761849743'){throw 'Unexpected KMD SYS'}
 $umd=(Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
 $icd=(Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
 if($umd -ne $cfg.'router.dll' -or $icd -ne '7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E'){throw 'Baseline UMD/ICD mismatch'}
 $os=Get-CimInstance Win32_OperatingSystem
 return @{utc=[DateTime]::UtcNow.ToString('o');boot=$os.LastBootUpTime.ToString('o');health=$health;clock=$clock;kmd=$kmd;umd=$umd;icd=$icd;dwm=@(Get-Process dwm | Select-Object -ExpandProperty Id);free_kib=$os.FreePhysicalMemory;total_kib=$os.TotalVisibleMemorySize}
}

$before=Snapshot
throw 'Unexpected GPU identity on CPU baseline'
