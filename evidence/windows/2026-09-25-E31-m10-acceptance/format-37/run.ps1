$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$out='C:\BC250\m10\color-oracle-37-release'
if(Test-Path "$out\native.exit"){throw 'Baseline already ran'}
if((Get-FileHash C:\BC250\m10\wsi-final\vulkan_radeon.dll).Hash -ne '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'){throw 'ICD mismatch'}
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read
if($LASTEXITCODE -ne 0){throw 'No health'}
$clock=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe clock read | Out-String
$clock
if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal mismatch'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No existing interactive user'}
@'
@echo off
set VK_DRIVER_FILES=C:\BC250\m10\wsi-final\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_VK_WSI_DEBUG=
set BC250_TRACE_SUBMITS=0
set BC250_TEST_FORMAT=37
set BC250_CUBE_CAPTURE_PREFIX=C:\BC250\m10\color-oracle-37-release\frame
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m10\color-oracle-37-release\color-oracle.exe --c 2 --width 321 --height 239 --suppress_popups > C:\BC250\m10\color-oracle-37-release\native.out 2> C:\BC250\m10\color-oracle-37-release\native.err
echo %ERRORLEVEL% > C:\BC250\m10\color-oracle-37-release\native.exit
'@ | Set-Content "$out\run.cmd" -Encoding ASCII
$action=New-ScheduledTaskAction -Execute conhost.exe -Argument '--headless cmd.exe /c C:\BC250\m10\color-oracle-37-release\run.cmd'
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 50)
Register-ScheduledTask -TaskName BC250-M10-Color37 -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask BC250-M10-Color37
$limit=(Get-Date).AddSeconds(55)
do{
 foreach($frame in @(1)){
  if((Test-Path "$out\frame-$frame.ready") -and -not (Test-Path "$out\frame-$frame.release")){
   Start-Sleep -Milliseconds 750
   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=1' -OutFile "$out\screen-$frame.png"
   Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=0.25&format=jpg&quality=45&overlay=1' -OutFile "$out\preview-$frame.jpg"
   [IO.File]::WriteAllText("$out\frame-$frame.release",'capture complete')
   'captured_frame='+$frame
  }
 }
 Start-Sleep -Milliseconds 100
}while(-not (Test-Path "$out\native.exit") -and (Get-Date) -lt $limit)
if(-not (Test-Path "$out\native.exit")){Stop-ScheduledTask BC250-M10-Color37; throw 'Baseline deadline; inspect before further GPU work'}
'native_exit='+(Get-Content "$out\native.exit")
if([int](Get-Content "$out\native.exit") -ne 0){throw 'Cube native failure'}
Get-Content "$out\native.out" -Tail 15
Get-Content "$out\native.err" -Tail 18
Unregister-ScheduledTask BC250-M10-Color37 -Confirm:$false
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read
'baseline_terminal'
