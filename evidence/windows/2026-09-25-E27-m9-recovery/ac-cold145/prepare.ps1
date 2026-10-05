$ErrorActionPreference='Stop'
$out='C:\BC250\m9\candidate07145\coldboot-01'
$cli='C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe'
$flags=Invoke-RestMethod http://127.0.0.1:2250/flags
if($null -eq $flags.stop -or $flags.stop){throw 'Owner STOP or missing flag'}
if(Test-Path "$out\before-driver.log"){throw 'Cold checkpoint already exists'}
$health=& $cli health read | Out-String
$health
if($LASTEXITCODE -ne 0 -or $health -notmatch 'flags=15 '){throw 'Current start not confirmed/healthy'}
$r=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
if($r.UnconfirmedStarts -ne 0 -or $r.EnableFullWddm -ne 2){throw 'Unexpected boot policy/budget'}
foreach($n in @('EnableNativeSmu','EnableMmio','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','EnableDcnWrite','EnableVidPnFlip','EnablePresentBlit')){if($r.$n -ne 1){throw "Missing gate $n"}}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'kmd_sha256='+$sha
if($sha -ne 'ED7B2E0735A41048DDA1428FB4A759C32193A231D5A26A5C2FEAD5D8407903CB'){throw 'SYS mismatch'}
if((Get-FileHash C:\BC250\mon\bc250mon.exe).Hash -ne '4F5597D3C34AF92A205CB3CC4AF031B1EB2983C6F477E89334EE04B5D2F7A875'){throw 'Monitor mismatch'}
if((Get-ScheduledTask -TaskName 'BC250 monitor overlay').State -ne 'Running'){throw 'Monitor task not running'}
& $cli clock read
if($LASTEXITCODE -ne 0){throw 'Native clock unavailable'}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-Process dwm,bc250mon | ForEach-Object {$_.ProcessName+' pid='+$_.Id+' start='+$_.StartTime.ToString('s')}
& $cli log | Out-File "$out\before-driver.log" -Encoding UTF8
if($LASTEXITCODE -ne 0){throw 'Cannot preserve driver log'}
$r | Format-List | Out-File "$out\before-settings.log" -Encoding UTF8
& powercfg.exe /a | Out-File "$out\power-states.log" -Encoding UTF8
$task='BC250 cold145 health recorder'
if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Recorder task already exists'}
$action=New-ScheduledTaskAction -Execute 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\BC250\m9\candidate07145\coldboot-01\startup-recorder.ps1'
$trigger=New-ScheduledTaskTrigger -AtStartup
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Settings $settings -User SYSTEM -RunLevel Highest | Out-Null
'checkpoint_ready='+(Get-Date).ToString('s')
