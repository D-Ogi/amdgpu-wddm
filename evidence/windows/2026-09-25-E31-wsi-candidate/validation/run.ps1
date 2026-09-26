$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$out='C:\BC250\m10\wsi-validation-01'
if(Test-Path "$out\native.exit"){throw 'Baseline already ran'}
if((Get-FileHash C:\BC250\m10\wsi-fifo\vulkan_radeon.dll).Hash -ne '3A03A1729F678A492D22220702E37A5BB4F3F9C2D23DE747075A139E20875F72'){throw 'ICD mismatch'}
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read
if($LASTEXITCODE -ne 0){throw 'No health'}
$clock=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe clock read | Out-String
$clock
if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal mismatch'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No existing interactive user'}
@'
@echo off
set VK_DRIVER_FILES=C:\BC250\m10\wsi-fifo\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver,layer
set VK_LAYER_PATH=C:\BC250\m10\vvl
set VK_LAYER_VALIDATE_SYNC=1
set MESA_VK_WSI_DEBUG=
set BC250_TRACE_SUBMITS=0
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m10\wsi-validation-01\vkcube.exe --validate --c 120 --width 640 --height 480 --suppress_popups > C:\BC250\m10\wsi-validation-01\native.out 2> C:\BC250\m10\wsi-validation-01\native.err
echo %ERRORLEVEL% > C:\BC250\m10\wsi-validation-01\native.exit
'@ | Set-Content "$out\run.cmd" -Encoding ASCII
$action=New-ScheduledTaskAction -Execute conhost.exe -Argument '--headless cmd.exe /c C:\BC250\m10\wsi-validation-01\run.cmd'
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 30)
Register-ScheduledTask -TaskName BC250-M10-Validation01 -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask BC250-M10-Validation01
$limit=(Get-Date).AddSeconds(35)
do{Start-Sleep -Seconds 1}while(-not (Test-Path "$out\native.exit") -and (Get-Date) -lt $limit)
if(-not (Test-Path "$out\native.exit")){Stop-ScheduledTask BC250-M10-Validation01; throw 'Baseline deadline; inspect before further GPU work'}
'native_exit='+(Get-Content "$out\native.exit")
if([int](Get-Content "$out\native.exit") -ne 0){throw 'Cube native failure'}
Get-Content "$out\native.out" -Tail 15
Get-Content "$out\native.err" -Tail 18
Unregister-ScheduledTask BC250-M10-Validation01 -Confirm:$false
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read
'baseline_terminal'
