$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07159'
$done=Join-Path $out 'worker-done.json'
if(Test-Path (Join-Path $out 'worker-start.json')){throw 'Worker already started; inspect existing job'}
@{pid=$PID;start=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o');utc=[DateTime]::UtcNow.ToString('o');boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'worker-start.json')
$code=1
try {
 $collector=Start-Process -FilePath "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$out\collect-startup.ps1") -WindowStyle Hidden -PassThru
 @{pid=$collector.Id;start=$collector.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'collector-start.json')
 $installer=Start-Process -FilePath "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$out\install159.ps1") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out\install-output.txt" -RedirectStandardError "$out\install-error.txt"
 @{pid=$installer.Id;start=$installer.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'installer-start.json')
 $installerHandle=$installer.Handle
 $installer.WaitForExit()
 $installer.Refresh()
 $code=$installer.ExitCode
} catch {
 $_ | Out-String | Set-Content (Join-Path $out 'worker-error.txt')
} finally {
 @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;pid=$PID} | ConvertTo-Json | Set-Content $done
}
exit $code
